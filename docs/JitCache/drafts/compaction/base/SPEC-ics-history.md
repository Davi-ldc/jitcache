# SPEC-ics history

Rationale and review records for [SPEC-ics.md](SPEC-ics.md). Nothing here is binding; where this file and the SPEC disagree, the SPEC wins.

## Draft 1 (2026-10-05)

Written from scratch against THREAD revision `83e03d4bfef09e944e0e3d2bd9bc49a8a7db3d5be567dd386a4f887e4cd2bc62`, with no earlier SPEC at the path. No review has run yet.

### Records hold producer facts

The first sketch stored consumer values: `repatchCount` already rewound, a "given up" bit, `countdown` omitted. That makes the format encode policy, and the twin rule ("must equal its capture record") would then compare live state with a value the producer never had. Storing the raw learning group plus three facts about the cases (count, megamorphic case listed, slow-operation kind) keeps the record a faithful observation, puts THREAD's transformations in one pure function, and lets T3 check that function independently from a JS encoding of the same tables. It costs one byte per IC more than the transformed form.

The two case facts are kept apart because the code produces every combination that matters: a folded get with `*Megamorphic`, a folded `instanceof` with `*GaveUp`, a get that folded and then gave up (no case, `*GaveUp`, `m_cacheType` still `Stub`), and a site that gave up on its eighth case (eight cases, `*GaveUp`).

### Dense records in native index spaces

Sparse records keyed by bytecode offset were considered for call links. A bytecode offset cannot be validated as an instruction boundary without walking the stream, while (opcode, metadata ID) is validated by comparing per-opcode counts, which prepare needs anyway. Dense records also make seeding a single walk beside `MetadataTable::forEach`, with no lookup, which matters for the installation bound. A call-link record is two bytes, so density costs little even for sites the producer never reached. The group table lists only opcodes present in the body, which keeps a small body's section near 40 bytes of overhead instead of 128.

### What the summary counts

THREAD gives the lane "the count of IC sites with cases" for the tie-break. Cases are `AccessCase`s, so the count covers property ICs only. Counting linked call sites as well was considered and rejected: the LLInt already links calls, so the capture at the end of `BaselineJITPlan::finalize` would score warm on call links while its property ICs are all cold, and the tie-break would stop distinguishing a warm `delta` capture from the first one.

### Every IC at a zero wait counter, every call site seen once

THREAD says "Each property IC comes back with ... its wait counter at zero" and "Call sites come back seen-once". Both sentences are general, and their stated purpose (the consumer's own case at the first visit, the first call linking natively) applies equally to sites the producer never reached. The narrower reading, restoring the captured `countdown` or `seenOnce` for unreached sites, would make those sites one trip slower than the reached ones for no observable gain, and it would need the record to keep `countdown`. The SPEC applies both rules to every site and records the captured `seenOnce` anyway, for the round trip and for later tiers.

### `resetByGC` travels

Nothing reads `resetByGC` today. It is learned per site, like `clearedByGC` on call links, which THREAD carries, and THREAD's opening says "Everything else the baseline phase learned travels too". It costs one bit.

### `m_cacheType` is left as installation builds it

Restoring `Stub` for sites that had cases would leave an IC at `Stub` with no case, which the status classes read as `Simple` with no variants, a state native code reaches only after a megamorphic give-up. Leaving the shape group alone makes a restored, considered IC identical to a native IC after a reset, which is what THREAD's example needs: `PutByStatus` answers `LikelyTakesSlowPath`, so the DFG emits a generic put instead of a `ForceOSRExit`. It also keeps "Restoration emits no IC code" literally true: attach touches only the learning group, `canBeMegamorphic` and, for given-up sites, `m_slowOperation`.

Draft 1 also claimed here that a site which gave up on its eighth case should stay given up in the consumer, and that it would still read `TakesSlowPath` "as the producer's would after its next visit". The second half was wrong: a producer visit with one of the cached shapes runs a handler and never reaches the `*GaveUp` operation, so the producer's `tookSlowPath` stays clear and its DFG reads the cases as variants. Revision 4 changes the rule for such sites.

### The folded `instanceof` site (provisional)

THREAD describes folded sites and given-up sites as different. `instanceof` is the one access type where the native fold also installs the `*GaveUp` operation (`tryCacheInstanceOf` returns `GiveUpOnCache` for `GeneratedMegamorphicCode`). Restoring it as given up would cost the consumer the inline prototype walk at that site for good and change `InstanceOfStatus` from `Megamorphic` to `TakesSlowPath`. Restoring it as folded reproduces the producer's state at the first visit. The SPEC classifies by the listed case and marks the choice provisional. Revision 4 replaces this special case with one rule for every site that gave up beside listed cases.

### Executable-allocation fault hooks (provisional)

THREAD Failures lists "IC stubs and handlers" among allocations whose failure raises a fault "before its effects are written". The effect of such a failure is a give-up, which is IC learning state, and the only three can-fail sites are in `InlineCacheCompiler`, which the lane already edits for E4. Leaving the hooks to the integrator would put a second part into the same file and functions. The entry point stays with the integrator, which owns the fault plumbing. Marked provisional because THREAD's Execution does not assign these sites.

### E1 without a write barrier

The LLInt's `op_super_construct` stores into `m_cachedCallee` without a barrier, unlike `slow_path_create_this`, which uses `WriteBarrier::set` for its own cached callee. A weak cache in an old CB that an eden collection does not revisit can keep a pointer to a cell that died in that eden, and the DFG reads the cache with `unvalidatedGet` and freezes it. That hazard is native and already present for every CB that ran in the LLInt. THREAD asks for the template to store `new.target` "as the LLInt does", so E1 matches the LLInt exactly, including the missing barrier and the varargs form's cell check. After E1, CBs born in baseline share the hazard with LLInt CBs. If the human wants it closed, the fix belongs in both tiers at once and outside this lane's mandate.

### One megamorphic predicate

Capture must decide whether a listed case is megamorphic. Copying the eight case types into the module would let the copy drift when WebKit adds a megamorphic case type. E4 moves the existing file-static predicate into `AccessCase` instead, so the fold and the capture share it. E3 sits in Repatch.cpp for the same reason: the operations per access type are already chosen there, and the new switch has no `default`, so a new access type breaks the build until it is classified.

### Strict and always-on checks

Structural checks on the section (sizes, counts, enum ranges, reserved bits, pairing with molds and metadata) always run: they verify received material, and a failure would otherwise mean reading or seeding out of bounds. Checks that a record obeys native invariants, that molds look as baseline emits them, and that the newborn CB is pristine verify assumptions, so they run under strict. Every strict check sits in prepare, before anything is written to the CB; nothing can be checked after native setup without leaving a half-installed CB, so the cold-IC state after setup is verified by the twin check in test builds instead.

`maxAccessVariantListSize` is free, so the case-count bound is checked only at capture, against the producer's own option.

### Locking

Capture and attach take `CodeBlock::m_lock` because every native reader of IC state on another thread takes it. Seeding takes no lock: native linking writes call-link fields without it, and the newborn CB is unreachable. Delta may call capture while the walk holds `CodeBlockSet::m_lock`; native code never nests the two locks the other way, so the order is safe.

### Options

THREAD names the cool-down pair as fixed. `useLLIntICs` and `forceICFailure` are added as fixed because each changes what a carried state means (a `Virtual` site or a give-up would record the option, not the site). The remaining IC options change only how the consumer caches next, so they are free.

## Revision 2 (review round 1, 2026-10-05)

Round 1 filed six blocker and major findings on three defects: two lenses found the first, three the second and one the third. All held up against the code; none needed THREAD to settle it.

### A CB can have no metadata table (two findings, blocker)

`UnlinkedMetadataTable::link` returns null unless `addEntry` or `addValueProfile` ran, and many opcodes carry no metadata (`enter`, `ret`, `mov`, `in_by_id`, `del_by_id`, `del_by_val`, `has_private_name`, `has_private_brand`), so a hot body such as `function has(o) { return "x" in o; }` gets a null `CodeBlock::m_metadata` while still having a property IC. `MetadataTable::forEach` reads the offset table through `this`, and Draft 1 called it unguarded in sizing, capture, A8, S4, seeding and the snapshot. Every native walker tests `m_metadata` first (`CodeBlock::forEachLLIntOrBaselineCallLinkInfo`, `CodeBlock::reconcileLLIntInlineCachesAtGCEnd`).

The fix routes every walk through one header (`ICSites.h`, section 4.6), so a walk added later cannot skip the test. The new live test T12 and the T3 corpus both include a body without a table, one of them with a property IC, which is the case that exercises the property-IC path while the call-link path is empty.

### A recapture dropped a restored fold (three findings, one blocker)

Attach restored a producer's fold only in `canBeMegamorphic`, and capture never read that bit, so a ConsumerProducer whose `delta` recaptured an imported body before a folded site's first visit wrote the site as unfolded. Draft 1's I5 even stated that outcome. The richer recapture replaces the saved body, so one generation was enough to lose the fold. The engine's only writers of the IC's bit copy a mold's bit, nothing writes a baseline mold's bit, `reset` leaves it alone, and `tryFoldToMegamorphic` is its only reader, so on a baseline IC the bit means exactly "a fold restored by an import" and capture can record it without ambiguity.

The record gains `canBeMegamorphic` as bit 1 of the renamed `foldBits` byte. The findings proposed the names `foldPending` and `foldsAtFirstCase`; the bit keeps the native field's name because the record holds producer facts and the fact is the field. Three choices depart from what one or another finding suggested:

- The fold test keeps `slowOperationKind == Megamorphic`, which the simplicity finding called redundant. It is redundant only under strict (C4, S1). Without strict, prepare derives records that break S1, and the derivation should read every fact that means a fold; a `*Megamorphic` operation is installed only by `PromoteToMegamorphic`. The SPEC's Notes carries the one-line reason.
- Given up and folded are disjoint: `givenUp` tests only the listed megamorphic case, and `foldsAtFirstCase = folded && !givenUp`. The simplicity finding's derivation set both for a site whose consumer replayed a restored fold and then gave up. The fold bit would be inert there (a `*GaveUp` operation never caches, and nothing resets such an IC back to `*Optimize`), but carrying it would make the restored states overlap and the recaptured record differ from its source without any behavioral reason. With disjoint states each restored IC is cold or considered, folded, or given up.
- I5 is now a property of two pure functions: `recapturedPropertyIC` and `recapturedCallLink` give the record a capture of the restored state yields, and deriving it again gives back the same consumer state. T7 checks that idempotence for every record, and the twin check captures every import right after attach and compares with the recaptured records, so every consumer test run verifies the property, T10 included. No finding asked for this. It catches any field that restore writes and capture does not read, which is the kind of defect these findings found.

T10 now runs three processes (Producer, ConsumerProducer, Consumer) and leaves the folded sites unvisited in the middle run, which is the scenario the findings described. Its second half, the tie-break on IC sites with cases, needed a body whose recapture is not richer; marking it `noDFG` keeps its baseline code from writing value or argument profiles, so only its ICs change.

### The test bindings had no shape (major)

Draft 1 named a shell binding and two hooks without signatures, JS shapes or the producer-to-consumer handoff, so the binding's author and the test author would have invented different shapes. The JS shape now belongs to the lane: `functionSnapshotBaselineICs` in `ICTwins.cpp` builds the object, and the integrator only registers it. R-INT-7 states the remaining shell functions and the sequence runner (roles per run, `arguments`, a shared scratch directory, the Off comparison) as requirements. A shared `resources/ics.js` isolates the scripts from those names, so a different choice by the integrator changes one file.

The T8 hook was dropped. Running prepare on an interpreted CB needs the body's prepared image and pending import, and what T8 tested, that an import never reaches a CB that ran in the LLInt, follows from where the integrator places the install points; S4 only re-checks it under strict. S4 is now a function of its own (`checkNewbornCodeBlock`), tested on live CBs in T12, and T8 tests what the lane does own: seeds land only in the CB that installs the import, checked across two realms that share UCBs through `runString`.

T7's parts that needed a newborn CB and a prepared image (A7, A8, S3) became checks over plain data (molds as `BaselineUnlinkedPropertyInlineCache` values, counts as an array), which is also a better structure for prepare: one function per check, composed in a fixed order. The checks that need live objects (S4, metadata-free bodies, capture determinism) moved to `ICLiveTests.cpp`, with a recipe built on `ScriptExecutable::newCodeBlockFor` for newborn CBs and `JIT::compileSync` for baseline ones, so R-INT-9 needs no lane-specific hook.

### Smaller corrections

Section 5.1 said capture allocates nothing; `PropertyInlineCache::listedAccessCases` returns a `Vector<AccessCase*, 16>`, which stays inline for a baseline list under the default `maxAccessVariantListSize` of 8. The SPEC now says capture allocates no cell, and no memory under default options. `jitcacheDelta()` exposes `delta` to test scripts, in test builds only. THREAD Session lets the host decide when to call `delta`, and in those builds the jsc shell is that host, which keeps the function consistent with "exposed only in C++".

## Revision 3 (review round 2, 2026-10-05)

Round 2 filed one major finding, against the JS tests. It held up against the code, and THREAD did not need to settle anything.

### Which capture the consumer imports (major)

Every body under test runs under `noDFG`, so its CB is `CannotCompile`, and the finding traced what that does to THREAD's scoring. The capture at the end of `BaselineJITPlan::finalize` commits first and counts no IC site with cases. A `delta` capture replaces it only by scoring higher, and richness counts no call-link or IC learning state. A body whose producer state lay only in call links, in given-up or non-cell ICs or in considered ICs without a case could therefore tie, and the counter would decide. Under `noDFG` the counter moves only when `op_enter` takes its slow path for a trap or for the CB's write barrier. T3, T4 and T5 would then fail or flake, and T4, T5 and T8 did not say that their producers call `delta` at all.

Each step checks out: `functionNoDFG` (jsc.cpp) sets `setNeverOptimize`, `DFG::mightCompileFunctionForCall` reads `isOkToOptimize`, `JIT::compileAndLinkWithoutFinalizing` clears `m_shouldEmitProfiling` and `m_canBeOptimized` for `CannotCompile`, `emitValueProfilingSite` and `emitArrayProfilingSiteWithCell` test `shouldEmitProfiling()`, `emit_op_enter` and `emit_op_loop_hint` add to the counter only under `canBeOptimized()`, and `op_enter_handlerGenerator` adds the entry increment on its slow path whenever `useDFGJIT` is on.

`noDFG` stays. Without it a body tiers up and stops being eligible, the thresholds that would delay that are fixed options, and the capability class is a baked fact that must agree between runs, which `noDFG` in every run guarantees.

The finding offered two fixes, and the SPEC takes the first. Each body whose `delta` capture a later run checks keeps an anchor, a property IC that lists a case when the body is passed to `delta`. Since what richness counts only accumulates, the `delta` capture wins on the IC count at the latest. `delta(...bodies)` in `resources/ics.js` checks the anchor before calling `jitcacheDelta()`, so a script that breaks the convention fails in its producer run every time, instead of importing the finalize capture when the collections happen to fall that way. `toBaseline` makes the warm-up explicit: the producer drives each site in baseline code, after the finalize capture. T4, T5 and T8 now describe their producer runs.

The second fix, comparing the consumer with the record the import used, was not taken. The twin check already makes that comparison on every import. And when the import used the cold finalize capture, T3 would pass while testing nothing about the capture of the states it drives.

The monotonicity argument needed `noInline` as well. A `noDFG` body is still an inlining candidate, because `DFG::inlineFunctionForCapabilityLevel` reads `isInliningCandidate` and not the callee's capability class. A DFG compile of a caller, `toBaseline`'s own loop included, could inline it. The calls it inlines would then bypass the body's baseline ICs, and the parser would drain the body's profiles on a compiler thread, which is the one way overlapping drains drop a prediction bit. With `noInline` only the collector drains a body under test, and its marking and finalization drains never overlap.

### T10's assertion on F proved less than it claimed

T10 checked that F's folded sites carry `canBeMegamorphic` in the Consumer. That holds whichever of F's captures the Consumer imports, because I5 makes the recapture of an untouched site derive the same consumer state as the record it came from. F now has a witness site that only the ConsumerProducer reaches, with a number as its base, so `sawNonCell` in the Consumer shows that F's recapture is the body it imported. T10 also states which body goes without an anchor in which run: F wins on richness in the ConsumerProducer although the saved body lists two IC sites with cases, and G wins on the IC count. I5's wording now says that the recapture commits only when it wins the scoring.

### Smaller corrections

T5's sentence could be read as giving its non-cell site `tookSlowPath`. Only the `*GaveUp` operations write that bit, so the site has `sawNonCell`, and T5 now names each bit with its site. The keep-alive convention now covers the callees of call links too, because a collection between the snapshot and `delta` would otherwise unlink a site whose callee died.

## Revision 4 (review round 3, 2026-10-05)

Round 3 filed two major findings. Both held up against the code. The second needs THREAD to settle it, so its rule is provisional.

### The ICs prepare had no code to read before `commit` (major)

`prepareBaselineICs` read the molds from a `const BaselineJITCode&`, and R-IMG-1 asked for "the `BaselineJITCode` setup will install". The Image SPEC the round reviewed built that object privately inside `prepareImage`, exposed only `moldCount()` and `commit`, and required `commit` after every lane's preparation. `commit` adds to the VM's code-size statistic and writes the JIT dump, so calling it first would leave those effects behind a later ICs failure. Following both SPECs as written, the integrator could not wire the install function.

The finding proposed a span of molds and a `PreparedImage::molds()` accessor. In the same round the Image lane, which owns that interface, added `const BaselineJITCode& PreparedImage::code() const`, valid until `commit` or destruction and the same object `commit` returns; its R-INT-7 passes `code()` to `prepareBaselineICs` and destroys the prepared image without `commit` when another lane's preparation fails. The SPEC therefore keeps its signature, rewrites R-IMG-1 against `code()` and places the call between `prepareImage` and `commit` in R-INT-4 and section 6. The whole object also serves better than a span: `commit` hands that same object to setup, so A7 pairs the records with exactly the molds setup turns into ICs, and the integrator has no way to pass molds from anywhere else. T12 already passes a CB's `BaselineJITCode` and needs no change. A Note in the SPEC records the choice.

### Sites that gave up beside their cases (major, THREAD gap)

The derivation set `givenUp` for every record with the `*GaveUp` operation and no megamorphic case. That included sites that gave up while still listing cases: the eighth case compiled as final code, or a `GiveUpOnCache` answer after earlier cases. In the producer those cases stay chained and keep serving their shapes, so `tookSlowPath` stays clear and the DFG reads the cases as variants. Restored given up, every one of those shapes calls C++ for the CB's whole life, since a `*GaveUp` operation never caches and an IC without a case holds nothing a reset could fire on, and the forced `tookSlowPath` turns the DFG's status into `TakesSlowPath`. Such sites are common: a `length` access over several receiver kinds, a by-value get mixing indexed and named cases, and any direct, private-name or delete site with eight shapes.

Each native fact checks out. `compileOneAccessCaseHandler` returns `GeneratedFinalCode` once the list reaches `maxAccessVariantListSize`; `addAccessCase` prepends that handler, because `generatedSomeCode()` includes final code; `tryCacheGetBy` then answers `GiveUpOnCache` on `shouldGiveUpNow()`, and `repatchGetBy` installs the `*GaveUp` operation beside the chained cases. The `*GaveUp` operations set `tookSlowPath` and cache nothing. `tryFoldToMegamorphic` keeps `length`, `name`, `prototype`, `__proto__` and indices out of the by-id get and `in` folds (`canUseMegamorphicGetById`), folds by-value gets only when every case is a plain load or miss, and has no arm for direct, private-name or delete access. `GetByStatus::computeForPropertyInlineCacheWithoutExitSiteFeedback` builds variants from the listed cases while `tookSlowPath` is clear.

One more native fact settled the rule. `PropertyInlineCache::reset` sends a site that holds cases back to its `*Optimize` operation (`resetGetBy` and its siblings), so natively a give-up lasts only as long as the cases beside it, while a give-up that holds no case is permanent, because nothing can reset it. JITCache drops every site's cases, so the give-up that survives that drop natively is exactly the one with no case. The SPEC takes the finding's rule, `givenUp = GaveUp && caseCount == 0`, and marks it provisional: THREAD Caches says without qualification that "a site that gave up comes back given up", while its opening and its rewind sentence assume the cases are re-cached. The rule also absorbs the folded-`instanceof` provisional, since that site lists `InstanceOfMegamorphic` and comes back folded with no special case. The round trip still holds, because a recaptured record lists no case and so holds the `*GaveUp` operation exactly when the site was given up.

The tests follow the rule. T3 adds a `get_by_id` site that meets a `GiveUpOnCache` answer while it lists two cases. T4 adds a `get_by_id` of `name` that gives up on its eighth case in the producer; in the consumer its first visit caches one case and its eighth gives up again, the producer's state. T5 adds the same kind of site, visited after its give-up only with its cached shapes, and checks that it reads `Simple` with `tookSlowPath` clear before any consumer visit. These sites read `name` because eight plain loads of an ordinary name fold into `LoadMegamorphic` instead of giving up. T7 adds zero and nonzero case counts to the combinations it derives.

## Revision 5 (system review round 1, 2026-10-06)

A major filed against THREAD named this lane's records among its causes: a site the producer saw with several shapes comes back with no case, a non-looping body whose captured progress had crossed reaches its first DFG compile at its second invocation (SPEC-cb.md, section 5.3), and that compile speculates on the shapes one invocation cached and then takes `BadCache` exits whose sites later outscore the producer's capture. The lane facts check out: cases never travel (section 1) and `caseCount` only rewinds `repatchCount` (section 6.3). The suggested fix would have this lane export, per body, whether a captured site listed two or more cases, for the integrator to pass to the CB lane's floor; THREAD states no such contract, so the CB lane keeps its uniform floor and marks the point provisional, and this lane exports nothing new. B4 now measures what would size the gap: how many cases such a site lists when the first DFG compile parses it, and the `BadCache` exits taken there.

## Revision 6 (system review round 2, 2026-10-06)

No finding was filed against this lane. A major against the CB and Image lanes changed the twin report this lane's runner reads: the Image lane's twin check is exact only with `useConcurrentJIT` off in both processes, so it now skips itself, with a reason, wherever that does not hold, and the report keeps skips apart from differences (SPEC-image.md, section 17.2). This lane's runs keep the option's default, which is on, so the Image check skips every import there, while this lane's own check, which runs before `installCode` on an unpublished CB, is unaffected. R-INT-7 now fails a run on a twin difference rather than on any report.

## Revision 7 (system review round 3, 2026-10-06)

No finding was filed against this lane, and nothing in it changed. Two findings cited it as evidence. The blocker against the CB lane's metadata walks pointed to `ICSites.h` as the guard to follow; the CB lane now has guarded walkers of its own, and section 4.6 stays as it is. The major against THREAD about plan-site faults named E5 as one of the two places where a lane placed its own fault call sites; SPEC-ucb.md R-INT-11 now gives the plan sites to the integrator, and E5 stays provisional as before.

## Revision 8 (five-part system review round 1, 2026-10-06)

No finding was filed against this lane. The major that system review round 1 filed against THREAD came back, with the point that B4 and the CB lane's bench measure one run's residue while the exit sites that residue induces can accumulate through ConsumerProducer recaptures. The lane facts are unchanged: cases never travel (section 1) and the wait counter comes back at zero (section 6.3). B4 now points to the integrator's IB10, which follows those sites across generations and joins each one to this lane's record at the same bytecode to count the sites whose record listed two or more cases. The lane exports nothing new: IB10 reads the case counts from committed `ICsBaseline` sections through `parseSection`.

## Revision 9 (five-part system review round 2, 2026-10-06)

One major, filed against this lane: task 6 bundled `ICTwins` with T2, a runner sequence, while the integrator's install glue waits in twins builds for the twin check. Verified. SPEC-integrator.md section 8.2 calls `checkRestoredBaselineICs` at step 14 in twins builds and its task 8 waits for "each lane's twin checks"; its task 12 registers `jitcacheICsSnapshot` and its task 13 is the runner, both after task 8. T2 snapshots through `resources/ics.js` (section 11.2), which task 7 wrote, so the old task 6 also needed a later task of this lane.

T2 moved to task 7, which already runs on the integrator's runner. Task 6 now needs tasks 2, 4 and 5 and the C++ test runner (the integrator's task 1), and lands before the integrator's tasks 8 and 12. Checking the plan for the same pattern inside the lane found T11 in task 4 although it reads `snapshotBaselineICs`, which task 6 defines; T11 moved to task 6. So that task 6 still lands with a test of its own, T13 checks the twin API on a live VM without the runner: a snapshot agrees field for field with the capture of the same CB, which section 11.1's shared reader promises; after a prepare, seed, setup and attach on the newborn CB of a never-called twin function, in the install function's order, the check reports nothing; and once the test clears one restored IC's `everConsidered`, the check reports that IC.

## Reviews

- Round 1: six findings (two blockers on the null metadata table, one blocker and two majors on the recaptured fold, one major on the test bindings), all accepted; revision 2 above.
- Round 2: one major finding (which capture the consumer imports when bodies run under `noDFG`), accepted; revision 3 above.
- Round 3: two major findings (the ICs prepare had no code to read before `commit`; sites that gave up beside their cases came back given up), both accepted; revision 4 above. The second is a THREAD gap and stays provisional.
- System review round 1: no finding against this lane; one major against THREAD involved its records and is a THREAD gap marked in SPEC-cb.md; B4 gained its measurement (revision 5 above).
- System review round 2: no finding against this lane; a major against the CB and Image lanes changed the twin report its runner reads (revision 6 above).
- System review round 3: no finding against this lane; a blocker against the CB lane and a major against THREAD cited it (revision 7 above).
- Five-part system review round 1: no finding against this lane; one major against THREAD involved its records and stays a THREAD gap; B4 points to the integrator's IB10 (revision 8 above).
- Five-part system review round 2: one major against this lane (T2 in the twin-check task, against the integrator's install glue), accepted; revision 9 above.

## Drain after the thread-prep run (THREAD sha256 `b7d45aea…73fb1e`)

THREAD settled both of the lane's provisional marks as the marks chose and changed three rules the lane follows. The run's one open finding for the lane was real. The drain ran in two passes; the second picked up at section 8.

1. Provisional marks, sections 6.3 and 7. THREAD Caches now says that only a site with no listed case comes back given up, and that a site that gave up with cases still listed (an eighth case compiled as final code, a `GiveUpOnCache` after earlier cases, a folded `instanceof`) comes back on its `*Optimize` slow call, empty, with its captured learning. The derivation already did this, and the folded `instanceof` also keeps `canBeMegamorphic` because it lists `InstanceOfMegamorphic`; `tryFoldToMegamorphic` returns that case for any first case once the bit is set, and `tryCacheInstanceOf` then answers `GiveUpOnCache`, which is the producer's state. THREAD Execution now gives the three `didFailToAllocate()` branches of `InlineCacheCompiler` to this lane, and E5 passes `ExecutableAllocationSite::InlineCacheHandler` to the integrator's `didFailExecutableAllocation(VM&, ExecutableAllocationSite)` (R-INT-6, M4). The marks and the list of provisional decisions are gone.
2. The polymorphic bit, sections 5.3, 5.6, 8 and 11.2. THREAD Restoration now has this lane hand the CB lane a bit that stops a body from carrying baseline counter progress when a property IC's record lists two or more cases, megamorphic folds aside, and leaves call sites to this SPEC. Capture and `summarizeBaselineICs` return `CaptureSummary`, with `hasPolymorphicSite` beside the summary, and `isPolymorphicPropertyIC` tests a record. Call sites set the bit too, because the code shows they meet the same exits. A `Polymorphic` site comes back seen once without its stub, so the consumer's first call links it monomorphically, `CallLinkStatus::computeFromCallLinkInfo` answers the one `lastSeenCallee`, and a DFG compile at the floor inlines it behind the `CheckIsConstant` of `emitFunctionChecks`, which exits with `BadConstantValue` when another callee comes. The producer's DFG read two or more counted edges from the stub, which give several variants or a slow path, and `handleInlining` makes a generic call outside the FTL. The consumer's exits lead to a jettison that records the exit site and counts a reoptimization, both of which travel. A stub with one slot (closures of one executable, merged by `variantListWithVariant`), a site with `clearedByGC`, a virtual site and a monomorphic one stay out. R-INT-2, R-INT-3 and R-CB-2 have the glue call this lane before the CB lane for the same CB in the same pause, as SPEC-cb.md R-INT-5 requires. T14 tests the bit on live CBs. B4 no longer points at a THREAD gap, and B6 measures the one case the rule leaves open: the bit reads the captured record, as THREAD's rule says, so a ConsumerProducer recapture taken before a polymorphic site has cached two cases again drops it.
3. Strict, sections 4.4 to 6.2, 10.1 and 11.2. Normal mode now checks only the artifact's integrity, which the integrator does, and strict is off by default. A1 to A8, C1, S1, S2 and the two checks on the integrator's call to capture run only under strict, and debug builds assert them; `parseSection` and `readBaselineICsSummary` take `StrictChecks`. T7 and T12 item 4 test both modes, and the bench measures the default.
4. Twin skips, R-INT-7 and section 11. A twin check that skips itself fails a run exactly when that run has concurrent JIT off. This lane's check never skips, since it reads a CB no other thread can reach before `installCode`.
5. Open finding, simplicity lens, blocker: C3, C4, C9 and S1 copied native operation and fold tables that no restore step needs. Verified. Only the `PromoteToMegamorphic` arms of `repatchGetBy` and its siblings install a `*Megamorphic` operation, and every path that removes the megamorphic case also replaces it (`resetGetBy` and its siblings, `repatchGetBySlowPathCall` and its siblings), so `slowOperationKind`'s `Megamorphic` value repeated `megamorphicCaseListed`. `tryFoldToMegamorphic` falls to its default and returns null for an access type without an arm, so C9 and S1's fold clause protected no restored state. And a table that missed an arm WebKit added would have made C3 a recording fault and S1 invalid material for sites the derivation restores correctly. Revision: the record's `slowOperationKind` and `foldBits` became `stateBits` with `holdsGaveUp`; E3 became `gaveUpOperationFor`; `foldsToMegamorphic`, C2 to C9, the old record-invariant checks and A5's operation clause went, and the mold and newborn checks became S1 and S2. The snapshot reports `holdsGaveUp`, and T3 now checks the E3 entries of the access types it drives, which C3 used to do. An operation the lane does not know restores as a reset leaves the IC, in both modes.
6. Smaller changes. E1 drops its conditional and M7: the Image lane reaches `JIT::compileOpCall` only through `CallLinkInfo::emitFastPath` and leaves the `super_construct` block to this lane. The `forceICFailure` row also names `linkPolymorphicCall`, which goes virtual under it. `parseSection` and `isPolymorphicPropertyIC` join the functions of section 8.1, for the bench.

## Kept minors from the thread-prep run

- ics batch 1.1 free IC limit options contradict the forceICFailure rationale: fixed, `maxAccessVariantListSize`, `thresholdForUndesiredMegamorphicAccessVariantListSize` and `maxPolymorphicCallVariantListSize` are fixed rows at 8, 0.5 and 8 (section 10.2), since a carried fold or `Virtual` records the limit as a give-up records `forceICFailure`; Bun sets none; the top-tier and Wasm call limits stay free, read only for callers this version does not carry; the byte truncation was already saturated.
- ics batch 1.2 three IC limit options classed free: fixed, with 1.1; the fixed `maxAccessVariantListSize` also keeps `listedAccessCases` inside its inline capacity, so section 5.1 now claims no allocation unconditionally.
- ics batch 1.3 caseCount is a byte while the option is free: fixed, `caseCount = cases.size()` is at most 8 under the fixed option (section 5.2); the saturation the drain added went with the free class.
- ics batch 1.4 caseCount bounded only by the free option: fixed, with 1.3.
- ics batch 1.5 caseCount truncates above 255: fixed, with 1.3.
- ics batch 1.6 caseCount uint8 against a free option: fixed, with 1.3.
- ics batch 1.7 case and variant limits free; add the bound to S1: fixed for the option rows; rejected for S1, since no restore step reads the bound and the drain removed strict checks no restore step needs.
- ics batch 1.8 three free IC options change carried fold and Virtual meaning: fixed, with 1.1.
- ics batch 1.9 fold and virtual-call limits classified free: fixed, with 1.1.
- ics batch 1.10 T9 can pass without exercising E5: fixed, R-INT-6 names the step `exec-alloc.ic-handler` (SPEC-integrator.md section 11.1), and T9 drives compiled IC stubs and fails unless some fuzz point reports that step.
- ics batch 2.1 L2 and 5.1 cite wrong native evidence: fixed, L2 cites `CodeBlockSet::add` in the two constructors and the End-phase removal, and 5.1 puts the clearing watchpoint on the VM's thread.
- ics batch 2.2 5.1 gives wrong reasons for capture consistency: fixed with 2.1, 5.1 rests consistency on the paused VM thread and names the unlocked writers, `repatchSlowPathCall` included.
- ics batch 2.3 L2 and 4.2 contradict the code: fixed, 4.2 names the decoded tables' constructors, `CachedMetadataSteps::build` and the clear in `finalize`; the locks.md report is left to the coordinator.
- ics batch 2.4 5.1 credits consistency to the CB lock: fixed with 2.2, the learning fields, `tookSlowPath` and `m_slowOperation` are named as written outside it.
- ics batch 2.5 four native justifications contradicted: fixed with 2.1 to 2.3, and section 6 no longer says attach locks "as native IC writers do".
- ics batch 2.6 L2 rests on `CodeBlockSet::remove` and omits two set-lock takers: fixed with 2.1, L2 names `VMTraps` and `SamplingProfiler` and now agrees with SPEC-integrator.md N7.
- ics batch 2.7 T3's collected callee gives `hasSeenClosure`: fixed, T3 collects a `new Function` callee together with its executable.
- ics batch 2.8 T3 never checks that the producer reached `clearedByGC`: fixed, the producer's snapshot must show `clearedByGC` at that site.
- ics batch 2.9 E5 omits the site argument the integrator requires: already fixed, E5, R-INT-6 and M4 pass `ExecutableAllocationSite::InlineCacheHandler`.
- ics batch 2.10 E5 and the Image hook call the one-argument form: already fixed, and SPEC-image.md and SPEC-ucb.md no longer use that form either.
- ics batch 3.1 E1 changes more native behavior than stated, T2 misses the LLInt-to-baseline path: fixed, E1 now covers every CB running baseline code, since the template stores back the value it loads (`JIT::compileOpCall`), and T2 gains `super-construct-tier-up.js`, which fails without E1.
- ics batch 3.2 E1 omits LLInt-born CBs and asks for a default-options test: fixed with 3.1; the tier-up script runs with default options, and task 2's existing `super` and `class` stress tests cover results across DFG compiles.
- ics batch 3.3 E1's effect reaches every baseline CB: fixed with 3.1.
- ics batch 3.4 E1 changes every baseline CB's cache, B5 lacks the case: fixed with 3.1, and B5 now includes a constructor that sees a second `new.target` after tier-up.
- ics batch 3.5 E3 copies installation's slow-operation mapping: already fixed, `slowOperationsFor` is gone and E3 defines only `gaveUpOperationFor`.
- ics batch 3.6 `foldsToMegamorphic` copies `tryFoldToMegamorphic`'s arms: already fixed, it is gone and capture classifies with `AccessCase::isMegamorphic` (E4).
- ics batch 3.7 E3 and `foldsToMegamorphic` copy native decision tables: already fixed, both named copies are gone; `gaveUpOperationFor` still names inline the operation five native give-up sites pass, which E3 states costs only speed on disagreement, and no strict check reads it, so no assertion was added.
- ics batch 3.8 Who lands E1 depends on another SPEC: already fixed, E1 says the lane applies it itself and M7 is gone.
- ics batch 3.9 M7's condition cannot be resolved by an implementer: already fixed with 3.8.
- ics batch 3.10 E1's landing route is conditional: already fixed with 3.8; section 13 keeps the `JITCall.cpp` row and task 2 lands E1.
- ics batch 4.1 T12 item 2's call script does not produce `caseCount` 2: fixed, T12 item 2 calls `has` three times in baseline code with shapes A, A and B, and says why: the installing LLInt call reaches no IC (`in_by_id` has no metadata) and the first baseline visit only spends `countdown`.
- ics batch 4.2 C++ harness lacks option initialization and build entries, T12.2 ambiguous: fixed; T12.2 with 4.1, and R-INT-9 now asks for `JSC::initialize()` once before any test, which S1 needs without a VM. The integrator's harness already does so and builds both test files (SPEC-integrator.harness.md section 8).
- ics batch 4.3 Two test statements fail if implemented literally: fixed with 4.1 for T12.2; T5's half was already fixed, the non-cell site expects `sawNonCell` with `tookSlowPath` clear.
- ics batch 4.4 T12 item 2 does not fix the call order: fixed with 4.1.
- ics batch 4.5 T12.2 does not pin the call order: fixed with 4.1.
- ics batch 4.6 T12 item 2's sequence does not guarantee `caseCount` 2: fixed with 4.1, and the record also expects `everConsidered`.
- ics batch 4.7 T12 item 2's three calls need not produce two cases: fixed with 4.1.
- ics batch 4.8 `jitcacheICsSnapshot` holds raw cells while it allocates the result: fixed, section 11.1 builds the result inside the `DeferGC` scope that takes the snapshot, so no collection reaches End before the result holds the cell.
- ics batch 4.9 `jitcacheICsSnapshot` can return a freed cell: fixed with 4.8.
- ics batch 4.10 The snapshot copies weakly held cells and L4 misstates where the lane reads cells: fixed with 4.8, and L4 now names the test-build read of the `super_construct` cache.
- ics batch 5.1 Test conventions leave the `Off` run undefined: fixed, section 11.2 says what a script does under `Off` (its Consumer path without `delta`, JSON or JITCache assertions; native assertions in every role); part (b), callee liveness, was already fixed.
- ics batch 5.2 Conventions break the integrator's heap oracle: fixed, section 11.2 adds R-ALL-4's `main(role, scratch, artifact)` rule and `jitcache-heap: off`, and R-INT-7 item 4 and T6 now describe the runner's oracle as SPEC-integrator.harness.md section 7.6 runs it (every JITCache run, heaps in twins mode, `arguments[2]` the artifact).
- ics batch 5.3 Same heap-oracle gap: fixed with 5.2.
- ics batch 5.4 The anchor argument ignores the plan's drain overlapping a marker's: fixed, the race holds (`BaselineJITPlan::compileInThreadImpl` drains before its safepoint without `CodeBlock::m_lock`, and `CodeBlock::visitChildren`'s first-visit drain runs outside it), so the producing runs of T3 to T5, T8 and T10 declare `--useConcurrentGC=false`, under which `SynchronousStopTheWorldMutatorScheduler` and `Heap::stopThePeriphery` keep markers from overlapping the plan's drain; the anchor paragraph and the Notes say so.

## Native-fidelity review after the minors (2026-10-07)

The review of the set's diff since `e1caff1` found no blocker and no major, and two minors; both held in the code.

- L4 said a collection alone replaces or frees a polymorphic call site's stub: fixed. The VM's thread replaces it in `linkPolymorphicCall` (`CallLinkInfo::setStub`) and drops it through `CallLinkInfo::reset`, which `setVirtualCall`, `revertCall` and `unlinkOrUpgradeImpl` reach; a collection's finalization unlinks it when a slot's callee died (`CallLinkInfo::reconcileWeakReferencesAtGCEnd`); and only a collection frees it, since `GCAwareJITStubRoutine::observeZeroRefCountImpl` only marks it jettisoned. L4 now says so; none of these runs during a capture, so its conclusion stands, matching section 5.1.
- R-INT-9 justified `JSC::initialize()` by T7's S1 reading `Options::repatchCountForCoolDown()`: fixed. S1 reads only the molds, `UnlinkedPropertyInlineCache` and `BaselineUnlinkedPropertyInlineCache` are plain data with no option-reading initializer, and only `PropertyInlineCache::considerRepatchingCacheImpl` reads the option, which T7 never calls. The requirement stays with its true reason: a test's VM needs the process-wide setup `JSC::initialize` performs (`Options::initialize`, `ExecutableAllocator::initialize`, the Structure address space and `LLInt::initialize`), as SPEC-integrator.harness.md section 8.2's `main` provides.
- Cross-set item from integrator batch 8.3 to 8.5: R-INT-7 item 4 said `ics/` runs keep `useConcurrentJIT` on, so the Image lane's twin check skipped their imports. SPEC-integrator.harness.md section 7.4 now turns concurrent JIT off in every directory unless a run sets it, so item 4 says the same, the image twin check compares `ics/` imports, and a skip fails the run. No ICs test needs worker compilation, and T9's fuzz index needs concurrency off, since the allocator's fuzz counter counts every thread's allocations.

## Compaction (2026-10-07)

SPEC-ics.md was shortened without changing what it requires, from 120080 to 113754 bytes; the set still has no sub-SPEC. No section was renumbered, and every identifier keeps its name: A1 to A8, C1, S1, S2, I1 to I5, E1 to E5, L1 to L5, R-INT-1 to R-INT-9, R-IMG-1, R-UCB-1, R-CB-1, R-CB-2, T1 to T14, B1 to B6, M1 to M6 and tasks 1 to 9. Most THREAD quotations were replaced by citations of their sections. Where the right column names a part of this file, that part holds the argument.

| old (HEAD) | new |
|---|---|
| SPEC-ics.md | SPEC-ics.md |
| SPEC-ics-history.md | unchanged above this section |
| preamble (two paragraphs) | preamble, one paragraph |
| 1 Scope | 1 |
| 2 Design in brief | 2, shortened to an overview; the record facts are stated in 5.2, the call-link fields in 5.4, the orders and the null-table walk in 4.2 and 4.6, and "prepare writes nothing, seed and attach cannot fail" in I1 and I2 |
| 3, 3.1 to 3.3 | 3, 3.1 to 3.3 |
| 4, 4.1 to 4.3 | 4, 4.1 to 4.3; 4.2's clause on how the walkers test the table moved to 4.6 |
| 4.4 Types | 4.4, which now lists every `ICSection.h` type and check declaration: it gained `StrictChecks`, `Summary` and `readBaselineICsSummary` from 5.6 and `Check` and `Invalid` from 6.2; the derivation and recapture functions stay in 6.3 |
| 4.5, 4.6 | 4.5, 4.6 |
| 5, 5.1 | 5, 5.1 |
| 5.2, items 1 to 5 | 5.2, items 1 to 5 |
| 5.2, "Four native facts shape what the record must hold" | 5.2, the list of states (a) to (d), which 6.3 cites instead of repeating them |
| 5.3 to 5.5 | 5.3 to 5.5; 5.4's null-table sentence is left to 4.2 and its stub-lifetime clause cites L4 |
| 5.6 Interface | 5.6, `ICCapture.h` declarations only; the `ICSection.h` ones moved to 4.4 |
| 6 Restoration, table of the three calls | 6; the position column is shortened and cites R-INT-4, which holds the full positions |
| 6.1 | 6.1 |
| 6.2 Checks and output of prepare | 6.2, heading names `jitcache/ICRestore.h`; `Check` and `Invalid` moved to 4.4 |
| 6.3 to 6.6 | 6.3 to 6.6 |
| 7 Native edits, E1 to E5 | 7, E1 to E5 |
| 7, New files table | 13, merged into the path table |
| 7, the `ics` naming rule for file-local helpers | 13, after the path table, naming the lane's `ICSection`, `ICSites`, `ICCapture`, `ICRestore` and `ICTwins` files |
| 8, 8.1 to 8.3 | 8, 8.1 to 8.3 |
| 9 | 9 |
| 10.1 | 10.1; its closing paragraph became one sentence citing 4.5, 5.5 and 6.1 |
| 10.2 | 10.2 |
| 11, 11.1, 11.2 | 11, 11.1, 11.2 |
| 12 to 14 | 12 to 14 |
| Notes, `prepareBaselineICs` takes the prepared `BaselineJITCode` | removed; 6.1 states the pairing (revision 4, "The ICs prepare had no code to read before `commit`") |
| Notes, T3 to T5 and T8 against the twin check | removed; 11.2's anchor paragraph states it (revision 3, "Which capture the consumer imports", and kept minor 5.4) |

Records above this section that cite the SPEC's Notes find their points where the last two rows say, and SPEC-integrator.md R-ALL-8's "section 7's `ics` prefix" now lives in section 13.

## Walkthrough reports

- `spec-ics-heap-comparison-exception.md`, R-INT-7 item 4 and T6 compare every `ics/` script's heap while section 11.2 lets a script declare `jitcache-heap: off`: fixed. SPEC-integrator.harness.md sections 7.2 and 7.6 compare the heap only for a script without that line, and both places now say so.
- THREAD Capture's tie-break, which now ranks a counter the polymorphic rule withholds above one that travels just before it compares progress, and the coordinator's last sentence of section 5.6: the sentence holds, since `cb.summary` carries `counterMode` (SPEC-cb.md sections 3.3 and 4.3, I16) and the integrator's `scoreSections` reads it for the saved body. Section 5.3's last paragraph, R-INT-3 and R-CB-2 still treated the bit as zero progress alone: fixed. A ConsumerProducer recapture that drops the bit replaces a saved body that has it only when it is richer, or is as rich and has more IC sites with cases; R-INT-3 scores the withheld counter on both sides; R-CB-2 asks the CB lane for the mark in both sections. The anchor argument of section 11.2, T10 and B6 stand, since richness and the IC count still come first.
- R-INT-7 item 4 restated the one-process twin-skip rule that THREAD Verification replaced with the importing process and every process whose captures it imports: fixed by the coordinator, which made the item cite THREAD's rule.

## Options table

- Section 10.2's seven rows and its free IC options moved to docs/JitCache/options.md, the one table the five sets share; section 10.2 points there, and section 5.2, R-INT-5 and M3 cite it. No option changed class, value or type.

## Runner directives

- R-ALL-4's directives, which SPEC-integrator.harness.md section 7.5 now enforces: T9's fuzzed Producer may report any of the five executable-allocation steps a producing run can reach, and the script declares each with `jitcache-expect-fault: 0`. No test checks anything new.
