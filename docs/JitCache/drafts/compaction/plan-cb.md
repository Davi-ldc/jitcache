# Compaction plan: the cb set

Files: `docs/JitCache/specs/SPEC-cb.md` (105,023 bytes, no sub-SPEC) and `docs/JitCache/specs/SPEC-cb-history.md` (66,470 bytes).

## What changes

The SPEC keeps every section number and every rule ID, so no citation of it from another file needs retargeting. Three kinds of edit make it shorter and easier to follow. The strict checks move to the step that runs them: SC1 to SC3 into the capture procedure (4.4) and S1 to S3 into `prepare` (5.2), which leaves 3.4 with the rules on the bytes (V1 to V15 and the summary rules). Eight passages that restate a rule another set holds, or receives in this pass, become citations (see "Cuts that cross sets"). Everything else is tightened sentence by sentence, keeping every fact that a rule, a test, a task or the WebKit bump review (9.3) uses.

The history becomes one entry per non-obvious decision, in the order the SPEC presents the decisions, each giving what first seemed right, the evidence that changed it and the intent that came out. Review rounds, routine fixes, the earlier compaction table and the report drains go; git keeps them. Nothing moves up into the SPEC, because every decision the history argues is already stated there; the SPEC gains a link beside each one instead.

The relayed request's "leave the history files alone" belonged to the Bun-flag binding update. Neither cb file mentions Bun flags or `BUN_JITCACHE_*`, so this plan applies the workflow's history rewrite as designed.

## Order of the reasoning

The top-level order stays, since it already runs from what travels, through the evidence and the format, to how state is captured and restored and how all of it is checked:

0. Opening: lane 3 of THREAD Execution, a design in brief, the history link, then the conventions ("VM thread", code location and namespace, `ENABLE(JIT)`, `ENABLE(DFG_JIT)` for the lazy-operand parts).
1. Scope: what travels (P1 to P12), what stays native and why, why P3 and P11 travel, the neighbouring lanes.
2. Native facts N1 to N11, in their current order.
3. Body file sections: the families (the format's index space), `cb.state`, `cb.summary`, then the validation of both sections.
4. Capture: hooks and context, reads, score, the procedure with SC1 to SC3 where they run, `scoreLive`, `decodeSummary`.
5. Restoration: call order, `prepare` with S1 to S3 and `seedLinkedState` with its realm step, `finishCounter`.
6. Interface: the two headers, R-UCB-1 and R-UCB-2, R-INT-1 to R-INT-12, the shared metadata entries.
7. Failures. 8. Invariants. 9. Native edits, manifest, bump review, owned paths. 10. Options. 11. Tests and bench. 12. Tasks.

Two placement changes serve this order. Today a reader of 3.4 meets SC2 and SC3 before capture exists, and 4.4 then says where they run, so each of those rules lives in two places; S1 to S3 are defined in 3.4 and run by 5.2 in the same way. After the move each check sits in the section of the task that implements it: task 2's validators in 3.4, task 3's capture checks in 4.4, task 5's install checks in 5.2. The opening gains a design in brief, as options.md's opening and SPEC-ics.md section 2 have, so the reader sees the shape before two sections of tables and facts: about four sentences, each ending in a pointer to the section that states it, adding no rule detail that section lacks (what is captured and at which two points, section 4; two dense sections in native index-space order, so validation compares counts and seeding walks in step, section 3; restoration around native setup, with the engine's own check re-slicing the counter above a floor and a body with a polymorphic site keeping setup's arming, section 5; strict validation, 3.4).

## Passages

### Opening

- Keep: "Lane 3 of THREAD Execution"; the definition of "VM thread"; `Source/JavaScriptCore/jitcache/`, namespace `JSC::JITCache`; `ENABLE(JIT)`; the lazy-operand parts under `ENABLE(DFG_JIT)`, which every supported target has.
- Rewrite the file and history sentence: this file is the lane's whole design, and SPEC-cb-history.md records why its non-obvious decisions are as they are and binds nothing.
- Add the design in brief described above.
- Cut "Code is cited by symbol and file." It describes the document, and SKILL.md already requires symbol citations. No requirement.

### 1. Scope

- The lead sentence shortens to an introduction of the table, since the opening now says what the lane does.
- P1 to P12 table: keep. P12's cell keeps its content in fewer words: the triple travels, the consumer keeps P = `m_totalCount + m_counter` and T and recomputes a finite threshold's slice (5.3), and a body with a polymorphic site carries none of it and keeps setup's arming.
- "The lane also writes its share of THREAD Capture's body summary (section 3.3)": keep.
- Stays-native table: keep every row with its reason; tighten the `m_didFailJITCompilation` and `m_capabilityLevelState` cells without dropping a clause (the integrator's fault before the flag is set, SC2's confirmation; setup recomputing the memo for function code, lazily filled otherwise, the class being a baked fact). Link the history entry "What stays local".
- P3 and P11 paragraph: keep; the four P11 writers become one clause. Link "What stays local".
- Neighbouring state (ICs, UCB and Image lanes): keep.

### 2. Native facts

- Lead line: cut "each verified in the code", a status line.
- N1: keep both paragraphs (the link state, and the null table with the walkers that test it); tighten.
- N2: keep the two merges, their running indexes, the builtin skip and the hardened `std::span::operator[]`. Compress the `get_by_val_with_this` evidence into one sentence that still names every reader 9.3 rechecks: `CodeBlock::getArrayProfile`, the route of the DFG parser, fixup and OSR exit, covers only the walks' opcodes; the DFG case reads only `GetByStatus`; the LLInt's slow path ignores the profile; nothing drains its samples; and the one read of its modes and pruning mark, `ArrayProfile::computeUpdatedPrediction(CodeBlock*, Structure*)` in `operationGetByValWithThisOptimize`, writes back into the same profile.
- N3: keep every writer and drainer, the finalizer's writes to P5 and P8, why no stopped-world writer overlaps a capture or reaches an unpublished CB, and the VM-thread-only lazy-operand append; tighten.
- N4: keep the six labelled sub-facts (slicing, process dependence, bounds, exits, setup, aging) and every formula; tighten wording only.
- N5, N6, N11: keep.
- N7: keep every fact (only OSR exit creates the profiles; the key's origin; the remap into the machine frame; locals past `numCalleeLocals()` and tmps at or above `maxNumCheckpointTmps`; hash and compare only; the holder's order carries no meaning; field equality; the two constructor assertions; no `GetLocal` on a constant register). Fold the remap mechanics (`InlineCallFrame::stackOffset`, `tmpOffset` set by `tmpOffsetForInlineeOf`, private tmps from `allocatePrivateTmps` past `numTmps()` in the root frame too) into one sentence: V13 and the corpus's inlined-callee subject need them.
- N8: keep the bound's facts (emission only for a non-empty all-constant literal, the recommended type, linking's seed, `updateProfile` only raising, the two readers reaching the hint through the instruction's own unchecked entry, what a lower hint does to the butterfly, raising being safe, the DFG reading the constant butterfly's own mode). Move to the history entry "The new_array_buffer hint bound" the sentence that native bytecode maps entries to instructions one to one, with its `BytecodeGenerator::rewind` and `BytecodeGeneratorification::run` evidence: since V8 checks each instruction's entry and covers an entry named by no instruction or by two, no rule depends on it.
- N9: keep the table and the facts about the DFG handlers and `canUseFastIterationMode`.
- N10: keep; drop the quoted assertion message, since the function names locate the assertion.

### 3. Body file sections

- Intro: keep the two sections, their opacity to the container, the section ids and checksums the integrator owns (R-INT-1) and native little-endian order with its `static_assert`. "No field holds an address, a cell, a `StructureID` or a time" stays as a pointer to I1, its home.
- 3.1: keep the family table, the generation paragraph with its `static_assert`s and the review item, the walker declarations, and the paragraph on their use, which becomes the one home of the walker-only rule (I15 cites it). Links: "Dense, positional sections" and "Bodies without a metadata table".
- 3.2: keep the code verbatim and the arrays table; the length sentence cites I10.
- 3.3: keep; link "Two sections".
- 3.4: retitle "Validation". Keep the intro (every rule here runs only with strict on; `validateState` is the one implementation of V1 to V15, which `prepare` applies to every `cb.state` it reads, failing with invalid material at the first violation, and SC1 to every one capture builds; normal mode's trust through R-UCB-2 and SC1), the paragraph on what the V-rules bound and guarantee, V1 to V15 and the `decodeSummary` paragraph with its three `CBCheck`s. Tighten V8's walk paragraph and V13 (one sentence each for the absent frame check, for duplicates and order, for the empty and deleted index values and for the checkpoint), keeping every bound. S1 to S3 and SC1 to SC3 leave for 5.2 and 4.4 with their lead-ins. Links: the intro to "Validation is strict-only", V8, V9 and V13 to their entries.

### 4. Capture

- 4.1: keep the two hooks, the glue's eligibility decision with the lane's recheck under SC2, the thread and deferral context, and the P12 race. The sentence on locks, cells, drains and buffers becomes one short sentence (no lock, no drain, no cell, only the two section buffers, charged first) citing I2, I3 and R-INT-2, which hold the detail.
- 4.2: keep.
- 4.3: keep the formulas. State once that `counterProgress` is THREAD Maintenance's P and the envelope's P, that `richnessUnits` is the lane's share of richness, each slot taken as the union of its CB and UCB copies, and why `counterWithheld` exists (THREAD Capture ranks a withheld counter above one that travels, just before progress). The glue's full order stays in R-INT-5; cut its restatement here. Link "Polymorphic bodies carry no counter progress".
- 4.4: step 2 takes SC2 and SC3 whole (their conditions, why they run before any read beyond step 1's counts, SC3's sentence that capture reads the UCB copies through the merges' hardened `operator[]` so a broken pairing fails natively in normal mode, the two faults, and the normal-mode `ASSERT` of both). Step 6 takes SC1 (the bytes pass `validateState` and S3 against their CB and the summary passes `validateSummary`; V8's F19 bound reads only the instructions, so it means the same for the live CB and allocates nothing; the fault). The `scoreLive` paragraph points to step 2 and keeps its normal-mode `ASSERT` of SC2 alone (see F2 below). Keep the `decodeSummary` paragraph.

### 5. Restoration

- 5.1: keep the seven steps, the context paragraph (deferrals, the CB unpublished until step 7, no lock or fence, the two native allocations and their publication) and the baked-fact paragraph. Links: steps 2 and 5 to "Tier-up history before setup, aging after", step 6 to "Twin checks run before installCode".
- 5.2: `prepare` takes S1 to S3 with their lead-in, run after V1 to V15 in the order S1, S2, S3; keep that normal mode runs none of them and cannot fail, the returned view of the borrowed span and that `prepare` allocates nothing. Keep the seed table and the field-by-field sentence. The realm step keeps each fact in fewer words: when it runs, why (the DFG's assertion, a consumer realm without `Map` or `Set`, the floor bringing a compile as early as the second call), what the getters allocate and when, that they generate no code, heap parity with a JITCache-off run, the deferral, demand only, and one realm sufficing. Links: the P3 row to "Lazy-operand keys", the realm step to "The realm step".
- 5.3: the pseudo-code stays verbatim. Merge the paragraph on a body with a polymorphic site into the opening paragraph's not-carried case, which already says setup's arming stands; keep that P10, P11 and every profile still travel, that `counterProgress` is zero, and that the floor's residue is the bench's. Keep the infinite-threshold paragraph, the resample and why `finishCounter` returns its decision. Links: "The counter travels raw" and "Polymorphic bodies carry no counter progress".

### 6. Interface

- 6.1: both code blocks verbatim; keep the exports paragraph.
- 6.2: R-UCB-1 cuts "if its SPEC names them otherwise, the integrator's derivation maps the names" and cites SPEC-ucb.md section 8.4, which adds both accessors under these names. R-UCB-2: keep.
- 6.3: R-INT-2 cuts "The names are the lane's working names; the integrator owns the API" and cites SPEC-integrator.md section 5.3. R-INT-6 cuts the clause that the Image lane's check skips whenever `useConcurrentJIT` is on and keeps the `TwinReport` sink, the call between `finishCounter` and `installCode`, the runner with strict on in both processes that fails on any reported difference, and the citation of THREAD Verification's skip rule. R-INT-12 links "The DFG plan-site fault comes first". The others keep, tightened.
- 6.4: keep, and cite SPEC-ics.md R-CB-1, the requirement it meets.

### 7. Failures

Keep the table and both closing paragraphs.

### 8. Invariants

Keep I1 to I16 with all their content. Tighten I1 and I3. I7 keeps every relation, since the twin check and U4 test against it. I15 keeps the null-table outcome and cites 3.1 for the walker-only rule.

### 9. Native edits and owned paths

- 9.1: keep; the paragraph after E3 links "Lazy-operand keys".
- 9.2: keep M1 to M3; tighten the Bun paragraph without dropping the three native APIs it names.
- 9.3: keep.
- 9.4: keep the lists of files and functions. The unified-sources paragraph becomes one sentence: file-local names follow SPEC-integrator.md R-ALL-8 with the prefix `cb`.

### 10. Options

Keep.

### 11. Tests and bench

- Intro: keep the three twin definitions (the capture record; for the slice, the answer native code gave `finishCounter`; setup's arming for a counter that does not travel), strict on in every test, `debug-local` with ASan and LSan, producer and consumer in separate processes through the runner (R-INT-6), and the remark that the lane's sections hold no address. The runner's own flags become citations: `--destroy-vm` on every jsc run (harness section 7.3) and the twins-mode `useConcurrentJIT` default for `cb/` (harness section 7.4). Keep the lane's rule that a subject needing it off in plain mode sets it in its own `jitcache-runs` line.
- 11.1: keep; the P12 bullet links "The counter travels raw".
- 11.2: keep the CB recipe, including "the test keeps it in a local and never installs it". U8 calls `prepare` and `seedLinkedState` on a newborn CB outside any deferral, which SPEC-ics.md section 11.2's "the test uses it inside that scope" does not allow, so this recipe is the lane's own and stays. Keep U1 to U11 with every case, mutation and expected `CBCheck`; tighten only the connecting prose.
- 11.3: the opening keeps its first sentence and its citations (R-ALL-4, harness section 7). The `Off`-role sentence keeps only the lane's clause and cites R-ALL-4 for the rest of the convention (cut 8), for example: "Under the `Off` role a script follows R-ALL-4 and neither reads nor writes `scratch`." Cut the restated oracle, the body-function form and the heap escape (cut 2). Keep every subject with its observation. The concurrency subject cites SPEC-image.md section 17.2 and harness section 7.4 for the Image skips. Link "Concurrency runs".
- 11.4: keep B1 to B4. The closing paragraph names SPEC-integrator.md IB10 as the owner of the floor's residue and keeps the lane's decisions: no native hook, the two native logs (`verboseOSR` in `operationOptimize`, `logCompilationChanges` in `DFG::compileImpl`) under options that leave baseline emission alone, and each import's `CBCounterRestore` as the input. Link "The bench needs no lane hook".

### 12. Tasks

Keep; link "Tasks and edit ownership".

## Cuts that cross sets

Each cut keeps a citation in SPEC-cb.md. The owner holds the rule in its current text, except in cut 8, where R-ALL-4 receives it in this pass.

1. R-INT-6 and the second concurrency run in 11.3: that the Image lane's twin check skips whenever `useConcurrentJIT` is on, and that those skips fail nothing in that run. Owners: SPEC-image.md section 17.2 (the check's two preconditions and its skip) and SPEC-integrator.harness.md section 7.4 (a run whose options turn concurrency on treats a skip as no difference). 11.3 keeps one citation of both.
2. 11.3's opening: the oracle's comparison (results, exceptions, stack traces and, in twins mode, the reachable heap), the `(function main(role, scratch, artifact) { ... })(...arguments)` body form with role-independent output and nothing left reachable, and the `// jitcache-heap: off` escape. Owners: SPEC-integrator.md R-ALL-4, and SPEC-integrator.harness.md sections 7.2 (`jitcache-heap: off`), 7.3 (the body function, now with a fourth parameter, `sequence`) and 7.6 (the oracle).
3. 9.4's closing paragraph: unified sources bundle the lane's files with other parts', so file-local helpers and constants take the prefix `cb` or a per-file named namespace, and nothing at namespace scope is `static` with a generic name. Owner: SPEC-integrator.md R-ALL-8, which names the `cb` prefix.
4. 11's opening: the runner's flags as restated for the lane, `jsc --destroy-vm` and, in twins mode, `useConcurrentJIT` off unless a run's options set it. Owners: SPEC-integrator.harness.md sections 7.3 (`--destroy-vm` on each jsc run of a sequence) and 7.4 (the default for `cb/`). The lane keeps `debug-local` with ASan and LSan, which no other set states for its tests.
5. R-UCB-1: "if its SPEC names them otherwise, the integrator's derivation maps the names". Owner: SPEC-ucb.md section 8.4, which adds `UnlinkedValueProfile::prediction()` and `UnlinkedArrayProfile::arrayProfileFlags()` under these names and calls them this R-UCB-1.
6. R-INT-2: "The names are the lane's working names; the integrator owns the API." Owner: SPEC-integrator.md section 5.3, which declares `ProducerBudget` with `tryCharge` and `release`.
7. 11.4's closing paragraph: that THREAD Restoration gives the floor's residue to the bench loop. Owner: SPEC-integrator.md IB10, which sizes the floor's residue in bodies that carry progress.
8. 11.3's `Off`-role sentence, except its clause "without reading or writing `scratch`": a script under `Off` runs its Consumer path without `delta` and without any assertion about imported or captured state, and assertions about native state run in every role. Owner: SPEC-integrator.md R-ALL-4 (new section 12), which takes this convention for every part's scripts as the ucb plan's move X1 (SPEC-ucb.md old 13.3, third bullet). The integrator plan places it there (its section 2, new 12) in a sentence that covers state JITCache imported, seeded, attached or captured and names results, a thrown error and `cachedDataRejected` as native assertions, so it holds everything in cb's sentence except the `scratch` clause, which the integrator plan's section 8 leaves with this lane.

Not a cut: 11.2's recipe for newborn and baseline CBs resembles SPEC-ics.md section 11.2's but differs in the newborn CB's lifetime (see 11.2 above), so it stays.

## Moves that cross sets

None.

## The history

### What moves up into the SPEC

Nothing. Each decision the history argues is stated in the SPEC with any reason an implementer needs; for example, 5.3 already says that a counter that does not travel keeps the arming setup made from the seeded reoptimization count, which is why P10 and P11 land before setup. The rewrite links each history entry beside its decision, as listed below.

### What the history keeps

A header of two sentences (why SPEC-cb.md decides as it does where a more obvious design came first; nothing here binds, and the SPEC wins where the two differ), then these entries in the SPEC's order. Plain-text headings give stable anchors. Each entry gives what first seemed right, the evidence and the intent, in a few sentences.

1. What stays local (SPEC 1). `m_hasBeenCompiledWithFTL` waits for the DFG capture; P3 and P11 travel although only optimizing tiers write them, because THREAD names both and the explicit statement wins over its general rule; `m_osrExitCounter` belongs to optimized CBs; buckets hold only pending samples; `ToThisClearedByGC` travels as scalar history beside a cell cache that does not. The `get_by_val_with_this` array profile first travelled as family A16, because the IC slow operation's classification looked like learned state; nothing outside that classification reads it, so it stays local under THREAD's "recomputes before anything reads it", richness agrees since the DFG never sees its flags, and dropping it renumbered the later families. Sources: "What waits for the DFG capture or stays local", Round 2 finding 4, kept minors 1.6 to 1.8.
2. Dense, positional sections (3.1, 3.2). Sparse keyed records would save bytes only for cold sites, and bodies are captured after their baseline compile, so dense arrays in native index-space order won: validation compares counts and seeding walks in step without search. The families are a table generated from the native macros, not (opcode, field) pairs in the file, since the build ID pins opcodes and layouts and generation keeps this order equal to the merge order. Source: "Why the state is dense and positional".
3. Two sections (3.3). Scoring reads only the summary, at a key's first scoring, so a separate section keeps that read small; the summary holds per-slot categories, as THREAD lists them, and the score is derived at decode, so no redundant scalar needs keeping consistent; one progress value serves both the tie-break and the envelope's P. The layout versions stay although nothing fails on them today, since every format the container and the UCB lane define carries one. Sources: "Why two sections", gap candidate 1, kept minor 3.3 (a).
4. Bodies without a metadata table (3.1, N1, I15). The lane counted zero entries without a table but read the table directly elsewhere; `UnlinkedMetadataTable::link` returns null for such a body, `MetadataTable::forEach` and `valueProfileForOffset` fault on null, and such bodies still reach baseline, so a producer would have crashed at the finalize capture of every hot function without metadata. Intent: two guarded walkers, used for every access. The ICs lane's walker was offered and declined so that neither lane's tasks wait on the other's header. Source: system review round 3, finding 1.
5. Validation is strict-only (3.4, 4.4, 5.2, 7). First the V-rules always ran, as safety bounds, and only plausibility checks were strict, so a native behaviour the SPEC had not modelled could not turn cache activity off for every user. THREAD Session then made normal mode check integrity only and trust the rest, so every V-rule, summary rule, S-check and SC-check runs with strict alone; in normal mode a broken pairing fails as it does natively, through the hardened `operator[]`, and SC2 stays strict because normal mode trusts the glue's eligibility, checked before any read. A saved summary that fails is invalid material, received bytes; the capture's pairing guard is a recording fault. Sources: "Validation split", drain item 3, kept minors 2.8 to 2.10, gap candidates 2 and 3.
6. The new_array_buffer hint bound (V8, N8). V8 first accepted any copy-on-write type in F19, though a hint below the literal's type makes the slow path or OSR materialization copy cells into an Int32 butterfly the collector never scans, or turn them into doubles. Reading the newborn CB's own hint at `prepare` was proposed and rejected: it equals the instruction's type only at link state, and SC1 needs the instruction anyway. An interim rule rejecting an entry named by no instruction or by two was dropped, since the first is never read and the second is checked against both; native bytecode maps them one to one anyway (the sentence moved out of N8, with its `rewind` and generatorification evidence). Intent: a per-instruction lower bound by `m_recommendedIndexingType`, walked in place with no scratch, in `prepare` and in SC1. Sources: Round 2 findings 1 to 3, Round 3 finding 3.
7. Iteration-mode masks (V9, N9). A 15-bit bound admitted bits the DFG cannot parse: its handlers emit one case per bit and assert that no failed block is left. Intent: each opcode's native writer mask. The reviewer's `0x7fff` for the async opcodes was declined because their handlers mask to the same bits the writers record, and the narrower sets keep stray bits away from `canUseFastIterationMode`. No cap at `maxNumberOfFastIterationModes`: more bits are a superset the engine handles. Source: Round 2 finding 6.
8. Lazy-operand keys (V13, N7, 5.2's P3 row, E3). V13 first bounded tmps by `maxNumCheckpointTmps` and a strict S4 checked the CB's own frame; keys name the machine frame of whichever compile inlined the CB, so with strict on SC1 would have ended production at the first `delta` of an inlined CB that saw an exit. Intent: only what the key's constructors assert, plus no constant register; a ceiling such as the 10-bit `tmpOffset` range was declined because an inlinee's private tmps add to it without bound. Keys refer to no other lane's content or body. Capture later sorted keys so that V13 could demand strict order in place of a uniqueness test; that went too, since no reader needs unique keys. A restore-only append was added to avoid `addOperandValueProfile`'s linear search and then removed: n is one CB's distinct exit keys, which OSR exit already searches the same way, and the extra engine edit had nothing to measure against. Sources: Round 1 findings 1 to 3, Round 3 finding 3, kept minors 4.7 and 4.9, "Native edits and the manifest", gap candidate 5.
9. The counter travels raw (5.3, I1, I7, 11.1). An early draft re-derived `setThreshold` in JITCache; handing the restored counter to `checkIfThresholdCrossedAndSet` removed a copy of engine logic a WebKit bump could diverge from. The infinite threshold stays out of that call, because `setThreshold` would drop the progress, and keeps its captured slice, whose producer ceiling only decides when the next check runs. I1 first claimed byte-identical captures; the truncated slice, the fractions in the total, the pool-dependent multiplier and drain timing all differ between processes, and a record of P and T alone cannot always be split back into an exact float and int32 pair, so the raw triple stayed and U4 checks that two splits restore the same counter. The twin first re-ran the native check, but the pool count is an atomic that JIT workers and other VMs change; `finishCounter` now returns its decision, which the twin compares with a pool-independent envelope, and a return value was preferred to a mutable field or out parameter because it keeps `finishCounter` `const` and feeds the bench too. Sources: "Why the counter travels raw and the engine re-slices it", Round 1 finding 5, Round 3 finding 1, kept minor 1.5.
10. Polymorphic bodies carry no counter progress (4.3, 5.3, I16). One floor of two entry increments first served every body, marked provisional. After an import, a non-looping body's first DFG compile saw one invocation of IC evidence, and the `BadCache` exits that followed entered the exit profile and rode on into later generations' recaptures; a larger floor for all bodies would have delayed those whose first invocation already caches every shape. THREAD settled it: a body whose captured IC lists two or more cases carries no progress, the ICs lane supplies the bit, setup's arming stands and the floor still serves bodies that carry progress. The score carries `counterWithheld` because THREAD ranks a withheld counter above one that travels, just before progress; the encoding (`NotCarried` 0) is safe because no binary reads a summary written while that byte was reserved. Sources: system review round 1, five-part review round 1, drain item 1, the tie-break walkthrough report.
11. Tier-up history before setup, aging after (5.1, 5.3, M1). Setup's `optimizeAfterWarmUp` reads the reoptimization count, so P11 lands before setup and setup arms what native code would for this history, which a counter that does not travel keeps; P10 lands in the same step. Setup samples aging from a fresh counter, and restored progress would read as activity and renew a lease the CB never earned, so `finishCounter` resamples; the field is CB heuristic state, hence this lane's. The twin needed a reader of the private sample (M1's twins-only getter) and compares with `float(count())`, as the old-age check does. Sources: "Why P11 is seeded before setup", "Why the aging resample belongs here", Round 1 finding 4, gap candidate 4.
12. The realm step (5.2, N10, I14). A seeded `FastMap` or `FastSet` bit trips the DFG's assertion in a realm that has not created a `Map` or `Set`, and a release build exits on every run. Stripping the bits would drop state THREAD carries and break the twin; a DFG edit would change the engine for a state native code cannot reach. Intent: call the native lazy getters, under the install deferral, only for a seed that carries the bit. Every other realm read behind the seeds was checked; typed-array modes need no step because the DFG handles a missing class. Source: Round 2 finding 7.
13. Twin checks run before installCode (5.1 step 6, N11). After `installCode` a concurrent marker can merge the UCB copies into P1 and P2 and drain P3, and imported UCB copies often hold bits the CB copy lacks, so a later check would fail at random. Source: Round 2 finding 5.
14. The DFG plan-site fault comes first (R-INT-12). A DFG plan out of executable memory defers P12 through its callback while the baseline CB stays the replacement, so a later `delta` captured the deferral and every consumer would have kept the body out of the DFG; no part owned the fault until THREAD gave the plan sites to the integrator, and R-INT-12 keeps the order. Sources: system review round 3 finding 2, drain item 2.
15. Concurrency runs (11.3). One run first asked every twin to pass beside concurrent DFG compiles, which the Image check cannot do; the Image check now tests its own preconditions and skips, and the lane runs two concurrency cases. Per-lane twin switches in the runner were declined: a run that forgot one would leave the Image check unsound. Source: system review round 2.
16. The bench needs no lane hook (11.4). The floor's residue and the time to the first DFG compile were first promised as lane bench items, though both happen outside every lane call. Native logs already report both events under options that leave baseline emission alone, and hooks in `operationOptimize` and elsewhere would put instrumentation into files no lane owns, for a measurement THREAD gives the bench. Source: Round 3 finding 2.
17. Tasks and edit ownership (9, 12). E1 to E3 touch classes no other lane edits, while `CodeBlock.h` is shared, so its additions go through the integrator's manifest. Concurrent tasks first wrote the same files and no task owned the shared header and validators; task 2 now writes both headers and the validators. The twin check and the corpus first shared a task that waited for the integrator's install glue, which in twins builds waits for the twin check; `verifyTwins` and its unit tests now land first, the corpus after the glue and runner, and the corpus needs no Bun host because every case runs in the jsc shell. Sources: "Native edits and the manifest", drain item 5, five-part review round 2.

### What the history drops

| current passage | where it goes |
|---|---|
| Authoring heading, its opening paragraph and every THREAD hash | dropped: provenance |
| "Validation split" | entries 5 and 8 |
| "Native edits and the manifest" | entries 8 and 17 |
| "Options" | dropped: options.md's rows for `minimumOptimizationDelay`, `maximumOptimizationDelay` and `thresholdForOptimizeSoon` carry the reasons |
| "Gap candidates checked and found settled" | item 1 to entry 3, items 2 and 3 to entry 5, item 4 to entry 11, item 5 to entry 8 |
| Round headers, finding counts and "None needed THREAD" lines | dropped: logs |
| Round 2's correction of I3 (`AssertNoGC` under deferral) | dropped: I3 and U8 state it |
| Drain item 4 (twin skips, relocation) | dropped: the SPEC cites THREAD's current rules |
| Kept minors 1.1 to 1.4 | dropped: routine corrections of native claims |
| Kept minor 1.5 | entry 9 |
| Kept minors 1.6 to 1.8 | entry 1 |
| Kept minors 1.9, 1.10 and 3.a | dropped: options.md's `thresholdForOptimizeSoon` row |
| Kept minors 2.1 to 2.7 | dropped: 11.1 states why LLInt caches are compared field by field |
| Kept minors 2.8 to 2.10 | entry 5 |
| Kept minors 3.b, 3.1, 3.2, 3.4 to 3.10 | dropped: V13 and U4 state their reasons |
| Kept minor 3.3 | (a) entry 3; (b) and (c) dropped |
| Kept minors 4.1 to 4.6 and 4.8 | dropped: test and dependency fixes the SPEC states |
| Kept minors 4.7 and 4.9 | entry 8 |
| "Native-fidelity review after the minors" | dropped: routine fixes |
| "Compaction (2026-10-07)" | dropped: a log |
| "Report drain (2026-10-07)" | dropped: a log |
| "Walkthrough reports", U5 item and skip-rule item | dropped: routine |
| "Walkthrough reports", tie-break item | entry 10 |
| "Options table" | dropped: a log |

## Map

### Section numbers

Every section keeps its number.

| old | new | action | note |
|---|---|---|---|
| 1, 2, 3, 3.1, 3.2, 3.3 | same | keep | |
| 3.4 | 3.4 | keep | retitled "Validation"; S1 to S3 and SC1 to SC3 leave |
| 4, 4.1, 4.2, 4.3 | same | keep | |
| 4.4 | 4.4 | keep | gains SC1 to SC3 |
| 5, 5.1 | same | keep | |
| 5.2 | 5.2 | keep | gains S1 to S3 |
| 5.3 | 5.3 | keep | |
| 6, 6.1, 6.2, 6.3, 6.4 | same | keep | |
| 7, 8 | same | keep | |
| 9, 9.1, 9.2, 9.3, 9.4 | same | keep | |
| 10 | 10 | keep | |
| 11, 11.1, 11.2 | same | keep | |
| 11.3 | 11.3 | keep | the `Off`-role sentence keeps only its `scratch` clause and cites R-ALL-4 (cut 8) |
| 11.4 | 11.4 | keep | |
| 12 | 12 | keep | |

### Rule IDs

No ID changes. The returned data lists each ID, and each range label that opens a table row, on its own row.

| old | home | action |
|---|---|---|
| P1 to P12 (row labels "P6..P9", "P10, P11", "P1 to P11") | 1 | keep |
| N1 to N11 | 2 | keep |
| A0 to A14 ("A0..A14"), A15, "A0..A15" | 3.1 | keep |
| F16 to F30 ("F16..F19", "F20..F23", "F24..F28", F29, F30) | 3.1 | keep |
| "F16..F18" (V8's first bullet) | 3.4 | keep |
| V1 to V15 | 3.4 | keep |
| S1, S2, S3 | 5.2 | move |
| SC1, SC2, SC3 | 4.4 | move |
| R-UCB-1, R-UCB-2 | 6.2 | keep |
| R-INT-1 to R-INT-12 | 6.3 | keep |
| I1 to I16 | 8 | keep |
| E1 to E3 | 9.1 | keep |
| M1 to M3 | 9.2 | keep |
| U1 to U11 | 11.2 | keep |
| B1 to B4 | 11.4 | keep |

## Found and left unfixed

- F1, THREAD gap. THREAD Session classes a failed strict check by what it rejects: bytes read from the artifact are invalid material, and what a capture builds from the producer's own state is a recording fault. S1 and S2 reject the consumer's newborn CB, which neither class names; section 7 makes them invalid material, as SPEC-ics.md does for its own newborn check.
- F2, possible inconsistency. In normal mode `capture` `ASSERT`s both SC2 and SC3 (4.4, step 2), while `scoreLive` `ASSERT`s SC2 alone, though both read the UCB copies at the same positions. The rewrite keeps both as written.
- F3, stale restatement. 11.3 writes the body as `(function main(role, scratch, artifact) ...)`, while harness section 7.3 now passes a fourth argument, `sequence`. A three-parameter function still runs, so nothing conflicts; cut 2 above leaves the harness's form as the only statement.

## Notes for the rewriter

- Keep every code block verbatim: the walker declarations, `cb.state` and `cb.summary` structs, `finishCounter`'s pseudo-code, the two headers, E1 to E3 and M1.
- When SC1 to SC3 and S1 to S3 move, keep their text, their order and the faults and `CBCheck` each returns; the `CBCheck` enum's comments stay as they are.
- 11.2 keeps "keeps it in a local and never installs it": U8 depends on it.
- Leave citations into other sets, THREAD, HARNESS.md and options.md as they are; a later pass retargets them.
- Cut 8 rests on the integrator plan, which the cross-check confirmed, and does not wait for SPEC-integrator.md to show the `Off`-role sentence in R-ALL-4. The workflow rewrites every set at the same time, so this set's rewriter and reviewers may read R-ALL-4 before the integrator's rewrite adds it. If the integrator set reverts to its snapshot, R-ALL-4 lacks the convention, and the global review, which checks every cut against its owner, reports cut 8.
- No heading or ID in the SPEC is renamed except 3.4's title, which no other file cites.
