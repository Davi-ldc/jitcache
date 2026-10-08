# Plan: compacting the integrator set

The set is SPEC-integrator.md, SPEC-integrator.container.md, SPEC-integrator.harness.md, SPEC-integrator.maintenance.md and the history SPEC-integrator-history.md. Old numbers are those of the copies in `docs/JitCache/drafts/compaction/base/`.

## 0. How to read this plan

Unqualified section numbers name SPEC-integrator.md. "container 7.2", "harness 7.7" and "maintenance 4.2" name the sub-SPECs. "new 4.3" marks a number in the new layout where the old one could be meant. Rule IDs stand alone. M1 to M6 name both manifest entries of SPEC-integrator.md and tests of the maintenance sub-SPEC; both keep their IDs, and citations say "manifest entry M3" or "maintenance test M3".

Five rules apply everywhere, beyond the passages listed below:

1. A sentence that restates THREAD keeps only what the integrator adds and cites THREAD for the rest.
2. Provenance and status words go: "already", "now", "was measured, not read", "(SPEC-ucb.md sections 5.3 and 6.3.1 already do)", "the lanes' frozen designs already provide". Where the history explains a decision, the SPEC links that history record by its title instead (section 6 names the link points).
3. A reason longer than one sentence goes to the history, unless an implementer reading the SPEC alone would otherwise get the decision wrong. This plan names the reasons that stay.
4. A rule stated twice in the set stays in one place, and the other place cites it. This plan names the place that keeps each.
5. A citation of one item of another set's rule names the item by its content, such as "R-INT-11's item on the per-VM `Twins` object", never by its position, because that set's rewrite may reorder the items.

Nothing below changes a decision. Where the plan found a contradiction or a gap, the text keeps its meaning and section 9 reports it. Code blocks keep their declarations verbatim; only the comments this plan names change. Step numbers inside sections (start's steps, the install function's steps, `commitCapture`'s steps, the writer's steps) do not change, and neither do task numbers.

## 1. Order of the reasoning

### SPEC-integrator.md

The old order opened with three tables of files and edits (old 3) whose rows only make sense after the design; it explained strict (old 11.2) after every lookup, install and capture step that passes it on; it kept the requirements on the other parts (old 7.4) inside the section on bodies, which is not their subject; and it closed the glue with a failure table (old 11.1) that repeats the outcome each step already states. The new order starts with what a host calls, then what `start` builds, then what reads it, and ends with where the edits land, the tests, the bench and the tasks.

| new | title | from | why it sits here |
|---|---|---|---|
| header | | preamble | the set's files, the history link, the citation conventions |
| 1 | Scope | 1 | what the integrator owns and an outline with pointers |
| 2 | Native facts | 2, N1 to N15 | evidence the design cites by ID; N16 to N25 serve only the harness and move there |
| 3 | Public interface | 4 | what a host calls |
| 4 | Per-VM state | 5 and 11.2, with a sentence of 3.2 (in 4.5) and two of 11.1 (in 4.2 and 4.4) | what `start` creates and every later section reads |
| 5 | Options and process facts | 6 | what `start` checks before it opens the artifact |
| 6 | Bodies | 7.1 to 7.3 | the interface the lanes read and write |
| 7 | Install glue | 8 | |
| 8 | Capture glue | 9, with 11.1's rule for a failed capture step (in 8.4) | |
| 9 | Plan-site faults | 10 | its `BaselineJITPlan::finalize` edit shows the fault and the capture hook, so it follows both |
| 10 | Locks, threads and GC | 12; 10's sentences on the plan sites' GC deferral; the sentences of 8.4, 9.5, 9.7 and 10 on Bun's `vm.Script` route | the integrator's locks and threads, and the GC paragraph that names each glue entry's deferral: it cites 7.4 and 8.7, which keep the install and capture context the lanes cite, and states the plan sites' deferral, which section 9 has no context subsection for, and Bun's `vm.Script` route, which crosses sections 7 to 9 |
| 11 | Hosts | 14 | |
| 12 | Requirements on the other parts | 7.4, with the ucb plan's X1 in R-ALL-4 | R-ALL-1 to R-ALL-8, which other sets cite by ID |
| 13 | Invariants | 13 | same number |
| 14 | Files, native edits and the manifest | 3, 15 | where the decisions land; old 3.2 already sent the hot files to old 15 |
| 15 | Tests | 16 | |
| 16 | Bench obligations and parameters | 17 | |
| 17 | Tasks | 18, with a clause of 5.1 in its opening paragraph | task numbers unchanged |

The from column gives old numbers: the sections each new section is built from, and the sentences it takes from other sections. Section 2 gives each passage's fate.

Subsections: 3.1 Types, 3.2 `start`, 3.3 `status`, 3.4 `delta`, 3.5 Exported headers; 4.1 `VMState`, 4.2 Activity and production, 4.3 Strict, 4.4 Producer context and budget, 4.5 Faults, 4.6 Teardown; 5.1 The fixed-option check, 5.2 Process facts; 6.1 Sections, 6.2 Lookup, 6.3 Commit; 7.1 Install points, 7.2 The install function, 7.3 Outcomes, 7.4 Context; 8.1 Eligibility, 8.2 Scores, 8.3 Kept summaries, 8.4 Building and committing one capture, 8.5 The finalize capture, 8.6 `delta`, 8.7 Context; 11.1 The jsc shell, 11.2 Bun; 14.1 New files, 14.2 Native edits in this repository, 14.3 Edits in `~/bun`, 14.4 Hot-file edits, 14.5 The lanes' other manifest entries; 15.1 C++, 15.2 JS, 15.3 Bun.

Strict sits at 4.3, after the two switches and before the budget, because sections 6 to 8 pass it to every lane call. The capture subsections keep their order; only their numbers drop by one.

### Container sub-SPEC

The order already follows the data from disk into a process: names, the lock file, the header, the body format, the objects a process keeps, the index, the reads, the writer. Old 9 (Filesystem errors) goes, since each of its rows restates a section, and old 10 (Tests) becomes 9.

### Harness sub-SPEC

The order stays: build, report, checks, placement, helpers, heap description, runner, C++ framework, bench report, event counts, pin comparison, kills, tests. Section 3 loses old 3.1 (Objects), whose every sentence restates another place, and old 3.2 and 3.3 merge into section 3 as its table and the image check, without subsections. N16 to N25 enter the sections they serve, before the text that cites them: N16 and N17 at the top of section 4, N21 to N25 at the top of section 10, N18 to N20 at the top of section 11.

### Maintenance sub-SPEC

Unchanged order and numbers.

### History

Decision records grouped in the new SPEC order: interface and per-VM state, the artifact, install and capture, hosts, test builds and the harness, options (section 6 of this plan).

## 2. SPEC-integrator.md, passage by passage

### Header (preamble)

- Keep what the SPEC is, that it indexes three sub-SPECs and that all four files bind, and the conventions: citation by symbol and file, "THREAD", "VM thread", the lane SPECs and "UCB R-INT-3".
- Replace "holds rationale and review records and binds nothing" with a link to the history as the record of the non-obvious decisions, binding nothing.
- Cut "Every new symbol lives in namespace `JSC::JITCache` under `Source/JavaScriptCore/jitcache/`" (owner THREAD Execution: "JITCache's code lives in `Source/JavaScriptCore/jitcache/`, in namespace `JSC::JITCache`"). Keep "unless a row of section 14 says otherwise" as a clause after the citation.

### 1 Scope

- Keep items 1 to 10, with new pointers.
- Cut the paragraph "The lanes own everything inside their sections and every native edit their SPECs list. The integrator decides only what it alone owns: ... The UCB lane owns the canonical key bytes the file name encodes." Owners: THREAD Execution (what the integrator decides, the lanes' requirements, the UCB lane's key bytes, and "calls the lanes in this document's order", so the glue's order is THREAD's), THREAD Storage (each section belongs to the lane that writes it). One sentence cites both and keeps that each lane owns the native edits its SPEC lists, which new 14.2's "No other part edits these functions" mirrors for the integrator.
- Keep "In outline", with the pointers 3.2 and 4, 6.2, 7, 8, and 4.2 and 4.5 (old 5.2 and 11).

### 2 Native facts

- Keep the opening sentence and N1 to N15. N16 to N25 move to the harness (section 4 of this plan).
- N3 and N9 overlap SPEC-ucb.md F24 but stay whole: they name the exact branches section 9 edits (the two `state.allocationFailed` branches, `Plan::finalize`'s order), which F24 does not.

### 3 Public interface (old 4)

- 3.1 (old 4.1). Keep the declarations. Cut the closing sentence "The API returns results and diagnostics and calls nothing back (THREAD Session)" (owner THREAD Session). Give `delta`'s declaration the comment that holding no JSC-internal lock is the caller's documented duty (THREAD Session), moved from old 4.4, so the exported header carries it.
- 3.2 (old 4.2). Keep steps 1 to 10 and the opening paragraph. Step 7: the Producer bullet keeps its outcome names (`start.artifact-exists`, `start.not-an-artifact`) and cites container 1.3 for the reset and the creation, which stops naming outcomes; the Consumer bullet keeps `start.artifact-missing`, `start.header` and `start.incompatible` and cites container 3.2 for what corrupt and incompatible mean and what the detail names; the last bullet keeps the call and that the VMs of a process share the object (II21), and cuts "the call builds and lists it when no VM of the process holds it, and otherwise returns it as its holders keep it" (owner container 5.1). Step 10 cites harness 2 for the twin report, since old harness 3.1 goes. The opening sentence keeps its wording (gap G1).
- 3.3 (old 4.3). Keep.
- 3.4 (old 4.4). Keep the rejection steps, "These are THREAD Session's requirements", the release call and the outcomes. The last sentence's duty moves into 3.1's comment (owner THREAD Session).
- 3.5 (old 4.5). Keep.

### 4 Per-VM state (old 5 and old 11.2)

- 4.1 (old 5.1). Keep the declaration, the `jitCacheState` paragraph and the parts table. The declaration's closing comment becomes a pointer: task 7 completes the private part (section 17). In the lifecycle paragraph keep the destruction order (the writer before the producer lock, the opened artifact and the budget) and the forward-declaration sentence. Cut "Only `start` creates a state and only `didFinalizeHeap` destroys one" (owner II1) and the reason the constructor, the destructor and the hooks live in `JITCacheAPI.cpp` and land in task 7 (owners: 14.1's `JITCacheAPI.h` row for the file; section 17's opening paragraph and task 7 for the task, the opening paragraph taking the clause "destroying a state runs every part's destructor, `ValidatedBody`'s included").
- 4.2 (old 5.2). Keep the table and both paragraphs. Cut "Pending imports stay attached when activity goes off and die with their UCBs (THREAD Restoration); records on existing `BaselineJITCode`s die with their code." (owner THREAD Failures: "pending imports and image records die with their owners, without a sweep"). Add one sentence from old 11.1's closing: after a recording fault a ConsumerProducer goes on importing.
- 4.3 Strict (old 11.2). Cut the first sentence (owner THREAD Session) and the definition of integrity (owner container 3.2 and 4.5; one clause cites them). Keep that strict adds the structure checks and that the integrator passes the session's strict to every lane call that takes it, with the list (Image R-INT-3, the CB and ICs calls of 7.2 and 8.4, the summary readers `scoreSections` calls). Keep "The integrator adds no assumption check of its own" and fold the three bullets into one sentence that names the mechanisms (container 8.2's reread and `writer.section`; 8.4 step 6; the producer lock). Keep the sentence that each lane decides its own builder's checks (SPEC-cb.md SC1). Cut the last sentence (owner THREAD Verification). Link the record "Strict: integrity in both modes, structure under strict".
- 4.4 (old 5.3). Keep the declarations, `tryCharge`'s and `producerContext`'s paragraphs. The `m_jitCacheRecordsImage` paragraph keeps the integrator's edit: `BaselineJITPlan` gains `bool m_jitCacheRecordsImage { false }` and `jitCacheRecordsImage()`, set by the constructor, which `jitCompileAndSetHeuristics` and `JIT::compileSync` both run on the VM thread, from `producerContext(vm)` and the registry's `keyOf`, which takes only its leaf lock (SPEC-ucb.md section 5.3). It cuts what SPEC-image.md R-INT-12 and section 4.1 state: the member is written once before the plan is enqueued and read without a lock, a VM without a state pays one null test, a UCB without a key when its CB compiles never gets one, and the twin's plan computes the flag the same way while its recorder ignores it. Keep the list of charges, the hash-table rule with its one-sentence WTF bound, the index-entry paragraph shortened to the charge, its release at the end of production and why the entry stays in the index, and the list of uncharged allocations. Add, from old 11.1's last row: the registry's and the index's allocations crash on exhaustion, as any native allocation does. Link the records "Charging hash tables and index entries" and "Which compilations record is decided at plan construction".
- 4.5 (old 5.4). Keep the declarations, the site table and the raise rules. The `InlineCacheHandler` row says "the three `didFailToAllocate()` branches of `InlineCacheCompiler` (SPEC-ics.md E5)" and drops their names (owner SPEC-ics.md E5). Cut "The other allocations that may fail, in Yarr, Bun's FFI thunks and stubs and WebAssembly, keep native behavior" (owner THREAD Failures, which new 9's FFI paragraph cites in place of this section) and its FTL FFI clause (owner new 9, which keeps it). Cut "It runs before the failure's effects are written, so no later capture contains them (THREAD Failures)" (owner THREAD Failures; II15 keeps the ID). Add, from old 3.2's `Debugger.cpp` row: `Debugger::attach` calls `JITCache::didAttachDebugger(m_vm)` right after `setShouldBuildPCToCodeOriginMapping` (N10). Link the records "The fault entry point names its site" and "Every refused production charge is `budget.limit`".
- 4.6 (old 5.5). Keep both hooks. Cut "Neither VM destruction nor process exit commits anything" (owner THREAD Failures; II19 keeps the ID) and the sentence on a process that exits without destroying its VM (owner container 2).

### 5 Options and process facts (old 6)

- 5.1 The fixed-option check (old 6.1, 6.2 and 6.3). One opening sentence replaces old 6.1 and 6.2: options.md lists the must-match and fixed options (THREAD Storage), the must-match values go into the header (container 3.1), and `start` walks the fixed table in its order. Cut old 6.1's restatement of the classes and of the incompatibility rule (owners THREAD Storage, options.md's opening, container 3.2) and old 6.2's account of how options.md's fixed table was assembled (owner options.md). Old 6.3 follows unchanged.
- 5.2 (old 6.4). Keep.

### 6 Bodies (old 7)

- 6.1 (old 7.1). Keep the enum, the table, the id layout and the tier paragraph. Cut "The container only locates and checksums each section, which is opaque bytes to it (THREAD Storage)" (owner THREAD Storage). Shorten the alignment sentence to what the lanes get, citing container 4.3 for the offsets. Keep the B7 sentence and the requirements it meets.
- 6.2 (old 7.2). Keep. The `bodyVersion` bullet's cadence clause shrinks to a citation of container 6.3 and II16. Link the records "The index holds tokens" and "Test bodies and a lookup override".
- 6.3 (old 7.3). Keep. Cut "It reads no lane format" (owners II5 and container 8.2). Link the record "The writer takes section sources and reads no lane format".

### 7 Install glue (old 8)

- 7.1 (old 8.1). Cut the restating opening (owner THREAD Restoration; one clause cites it). Keep the code, the `compileSync` paragraph and the `shouldJIT` edit. Give the comment "the import calls installCode itself" its reason in one clause: the install function ends with `installCode`, last in THREAD Restoration's order. Link the record "One install function with a point argument".
- 7.2 (old 8.2). Keep steps 1 to 19 and the closing paragraph. Step 7 keeps the budget rule and one sentence of reason; the rest goes to the record "A ConsumerProducer rebuilds the image record only while production is active". Step 15 keeps the call and the twins call and cuts the account of what `finishCounter` does (owner SPEC-cb.md section 5.3). Step 18 keeps the stash, the drop and the bench timing, and cites IB1 for why the drop is timed apart.
- 7.3 (old 8.3). Keep.
- 7.4 (old 8.4). Keep the conditions the install function runs under, which SPEC-ics.md section 6 cites in place of its own copy (plan-ics.md section 3): the VM thread with the API lock and heap access; the two GC deferrals, `prepareForExecutionImpl`'s `DeferGCForAWhile` (N1), which the LLInt-off route reaches through `setupJIT`, and the function's own; that no stopped-world GC phase starts while it runs and nothing it calls starts or waits for a collection or releases heap access; that the newborn CB stays unpublished until step 16, so no marker or compiler thread reads what steps 11 to 15 write and the lanes add no fence; and the locks its callees take. The sentence on Bun's `vm.Script` route merges into section 10's statement of that route, which 7.4 cites.

### 8 Capture glue (old 9)

- 8.1 (old 9.1). Item 2 keeps the `isImageCapturable` call and "THREAD Capture's 'shareable and recorded'", and cuts when a record is `Complete` (owner SPEC-image.md section 10 and section 4.1).
- 8.2 (old 9.2). The comment above `beats` states how the code realizes THREAD Capture's order: lexicographic over the fields in declaration order, a true `counterWithheld` above a false one, a candidate wins only when strictly greater, and a tie keeps the saved body. It no longer restates the order. Keep the rest.
- 8.3 (old 9.3). Keep. Link the record "The scoring read opens by name".
- 8.4 (old 9.4). Keep steps 1 to 10. Step 6 keeps where the committed score comes from and the re-test, with one sentence of reason: a marker's or compiler thread's drain between the scoring and the build can add bits or drop them (profiles.md, "Drain"). The argument that each lane's score describes its bytes (SPEC-cb.md I8, SPEC-ics.md section 5.6, SPEC-ucb.md F17) goes to the record "The kept score comes from the builders". Add, from old 11.1's capture rows: a failure at any step writes nothing, destroys the lanes' outputs, which releases their charges, and frees the ICs buffer and releases its charge. Keep the order paragraph's two reasons and the one-commit sentence; cut "Everything happens before control returns to JS (THREAD Capture)" (owner THREAD Capture).
- 8.5 (old 9.5). Keep. Cut "The plan's CB is captured with the LLInt phase's feedback and call links, cold ICs and a fresh baseline counter (THREAD Capture)" (owner THREAD Capture). The sentence that a plan finalized for a CB whose UCB has no record, such as Bun's `vm.Script` route's, fails step 4 merges into section 10's statement of that route, which 8.5 cites; 8.1's item 3 already holds the general rule.
- 8.6 (old 9.6). Keep.
- 8.7 (old 9.7). Keep the conditions capture runs under, which SPEC-ics.md section 5.1 cites with 8.1, 8.5 and 8.6 in place of its table of capture points (plan-ics.md section 3): the VM thread with the API lock and heap access, JS paused and no collector phase on the thread; the finalize capture inside door 1's deferral (N8; install.md, "Conditions around installation") and `delta` inside its own; that it calls nothing that drains a profile, materializes a property table, allocates a cell or stops for the collector, and never finalizes a plan; and that its only file I/O, the writer's and the saved body's read, touches no heap. The sentence on Bun's `vm.Script` route merges into section 10's statement of that route, which 8.7 cites. The nesting argument shrinks to its premises (no callback, no plan finalization, no CodeBlock walk with plan completion, the writer's file I/O) with a link to the record "Captures cannot nest"; the `ASSERT_ENABLED` check stays.

### 9 Plan-site faults (old 10)

- Cut the restating opening (owners THREAD Execution and THREAD Failures; one clause cites both). Keep "These plan sites meet UCB R-INT-11 and CB R-INT-12; section 4.5 lists the other sites."
- Keep the two code edits and the DFG and FTL edits. Of the two sentences after the `BaselineJITPlan::finalize` edit, the one saying that Bun's `vm.Script` route reaches the same function through `JIT::compileSync` (N2) merges into section 10's statement of that route, and "Task 9 passes a null timing until task 16 adds `m_jitCacheMeasure` and the `jitCacheTiming` local" is cut (owners tasks 9 and 16).
- In the paragraph after the DFG and FTL edits, keep the callback's effects and why the fault precedes them, the twins count, the worklist lock ordering the flag's write before its read, the plans that fail for other reasons, and that both calls run inside the plan's finalization with no worklist lock held, which `didFailExecutableAllocation`'s contract allows (4.5). Its sentences from "On the engine's routes a GC deferral covers that finalization" to its end move to section 10: the plan sites' deferral, Bun's `vm.Script` route and why neither function assumes a deferral. Section 9 cites section 10 for them.
- Keep the FFI paragraph, which becomes the one place for that case (4.5's plan-site row cites section 9). Its closing clause, "as do the other allocations section 5.4 names", would name nothing once 4.5 drops that list, so it cites THREAD Failures, which holds the list: "Bun's FFI thunks and stubs outside a plan keep native behavior, as do the other allocations THREAD Failures lists."

### 10 Locks, threads and GC (old 12, with sentences of old 8.4, 9.5, 9.7 and 10)

- Keep the lock table, with "taken by" shortened to the operations and container pointers.
- Keep the first sentence after the table, which says both in-process locks are leaves and defines the term. Replace the second, which lists what runs under each lock, with a citation of container 5.1 and 6.4, which hold those lists (owner container 5.1 and 6.4). Keep the consequence that a thread holding one never waits on another JSC or JITCache lock, and the `VMState` sentence.
- Keep the lock-order bullets.
- Threads: keep. Cut "The integrator adds no thread and never pauses a GC or compiler thread (THREAD's opening)" (owner THREAD's opening).
- GC: keep the first sentence, that `start`, `status`, the index and the writer allocate no cell. The rest of the paragraph names the deferral each glue entry runs under: the install function, the two of 7.4; capture, door 1's or `delta`'s (8.7); the plan sites, stated here from old 10: `DeferGC` in `completeAllReadyPlansForVM` (N8, N9), or, when `JITWorklist::enqueue` finalizes on the spot with concurrent JIT off, the `DeferGCForAWhile` its caller holds (`jitCompileAndSetHeuristics`, `operationOptimize`, the FTL tier-up operations), as `prepareForExecutionImpl`'s does for `setupJIT`; and the teardown hooks, after `VM::~VM` has deferred GC for good (N5).
- Then one statement of Bun's `vm.Script` route, merged from the sentences of old 8.4, 9.5, 9.7 and 10 that state it. The route calls `JIT::compileSync` after its own `DeferGC` has closed (N2; install.md, "Embedder routes"), so the glue it reaches runs with no deferral, and none of it needs one. With the LLInt off it reaches the install function (7.1), whose step 1 makes only its loads and one registry lookup and returns `NotInstalled`, because the UCB Bun's decode yields has no record (SPEC-ucb.md section 6.2.6). Its `BaselineJITPlan::finalize` reaches either the finalize hook, whose steps 1 to 4 touch no heap and whose step 4 fails for a CB whose UCB has no record, or, when the plan fails, the fault entry point, which allocates nothing and touches no heap (4.5); neither function assumes a deferral. 7.4, 8.5, 8.7, 9 and II14 cite this statement.
- Fences: keep.

### 11 Hosts (old 14)

- Cut the introduction (owner THREAD Session); a one-line pointer to THREAD Session stays.
- 11.1 (old 14.1). Keep.
- 11.2 (old 14.2). Keep the three bullets and the build-id paragraph. In `configureVM`'s twins sub-bullet, the `recordVMLayout` call for the main thread's VM cites harness 4, which takes the clause "whether or not JITCache is configured"; the image-hook call in `JSCInitialize` stays here. The at-exit bullet keeps one sentence for why it flushes the bench report in every role (`BUN_DESTRUCT_VM_ON_EXIT`) and one for why the call is the host's idle point and no implicit `delta`. Link the records "Bun" and "Bun's twins build reaches only exported headers".

### 12 Requirements on the other parts (old 7.4)

- Keep R-ALL-1 to R-ALL-8. Drop the status words of R-ALL-5 ("already do"), R-ALL-6 ("already declares") and R-ALL-8 ("SPEC-ics.md section 13's `ics` prefix meets this"); R-ALL-8 cites 14.1 for the integrator's own prefix.
- R-ALL-4 takes the `Off`-role convention that SPEC-ucb.md (old 13.3, third bullet), SPEC-image.md (old 17.1), SPEC-cb.md (old 11.3) and SPEC-ics.md (old 11.2) each state for their own scripts. The ucb plan moves it here as its X1 (section 8). It follows the sentence that ends "receives the role and paths as parameters" and reads: "Under the `Off` role, which the oracle's runs pass (harness sub-SPEC section 7.6), a script runs its Consumer path without `delta` and without any assertion about the state JITCache imported, seeded, attached or captured; assertions about native behavior, such as results, a thrown error or Bun's `cachedDataRejected`, run in every role." The sentence is the ucb bullet without its one lane clause, and it covers everything the other three texts say outside their own clauses, which stay with their lanes (section 8).
- This changes no decision. Each lane added its copy to meet R-ALL-4 and the oracle, as the lanes' histories record, and the oracle runs every part's scripts under `Off`, the integrator's included. In such a run `jitcacheDelta()` throws `delta.unconfigured` (harness 5.2) and nothing installs, so a `delta` call or an assertion about imported state would fail there.

### 13 Invariants

- Keep II1 to II23, each as its property with a pointer to the section that maintains it; reasons go. II14 points to 8.7 and 10. II19 cites THREAD Failures.

### 14 Files, native edits and the manifest (old 3 and old 15)

- Opening: old 15's sentence on hot files and the tasks that apply them, and old 3.2's "No other part edits these functions."
- 14.1 (old 3.1). Keep the table, the sentences on exported, private and lane-included headers, and the files outside `jitcache/`. The unified-sources paragraph becomes one sentence holding the integrator's files to R-ALL-8 with the prefix `integrator` or a per-file named namespace such as `JSC::JITCache::ContainerInternal` (owner of the reason: R-ALL-8).
- 14.2 (old 3.2). Keep as the index of edits; each row points to the section that specifies its edit. The `Debugger.cpp` row points to 4.5, which now holds its placement. Context cells that restate harness 9.3 (the `installCode` row) shrink to the pointer.
- 14.3 (old 3.3). Keep, rows pointing to 11.2, harness 4, harness 5.3, harness 10.3 and maintenance 5. Keep the `build.ts` and `CLAUDE.md` paragraph.
- 14.4 (old 15.1). Keep M1 to M10. M1 names the lanes' entries (SPEC-ucb.md M1, SPEC-image.md M1, SPEC-cb.md M2, SPEC-ics.md M1) instead of copying their file lists (owners those entries), and keeps the integrator's own files, the twins note, the absence of tests from `Sources.txt` and the stub rule.
- 14.5 (old 15.2). Keep, with the Image M8 sentence.

### 15 Tests (old 16)

- Introduction: keep what the integrator's tests check, where they run, and that `integrator/` scripts run in the twins build only. Cut "Every test runs with strict on (THREAD Verification)" (owner THREAD Verification); keep that the runner passes `--jitcache-strict=1` (harness 7.3), that a C++ test that configures a VM sets `Config::strict`, and the default-mode sentence.
- 15.1 (old 16.1). Keep.
- 15.2 (old 16.2). Keep every row, its checks and its directives. `exec-alloc-faults.js` drops the reasons the baseline-plan, FTL-plan and MathIC sites need no further evidence and the account of which runs write a deferred counter natively; it keeps the constraint that the script's bodies stay below the reoptimization count at which `adjustedCounterValue` clips and have no polymorphic site (SPEC-cb.md I16). `exec-alloc-ic-marker.js` keeps one clause for what a given-up site would mean. The dropped reasons go to the record "What the executable-allocation fault tests can see".
- 15.3 (old 16.3). Keep.

### 16 Bench obligations and parameters (old 17)

- Introduction: cut "Every measurement runs with strict at its default, off (THREAD Verification)" (owner THREAD Verification).
- IB3: keep what each event contributes; cite THREAD's opening for the definition of both sides instead of restating it.
- IB10: cut the first sentence's restatement of the polymorphic rule (owners THREAD Restoration; SPEC-ics.md section 5.3 for the call-site bit); keep what IB10 checks, chains and records.
- Keep the other obligations, the "first change to measure" notes of IB1, IB2 and IB7, and the parameter table.

### 17 Tasks (old 18)

- Keep tasks 0 to 17 with their numbers and contents, with new pointers. The first paragraph's lifecycle sentence takes the reason from old 5.1.

## 3. Container sub-SPEC

- Preamble: keep; link the history.
- 1.1: keep; link the record "Body files are named by the key's hex". 1.2: keep.
- 1.3: keep the procedure. Step 1's "`start` rejects at `start.not-an-artifact` and changes nothing" becomes "the creation stops and changes nothing" (owner of the outcome: SPEC-integrator.md new 3.2, step 7). The last line keeps "whatever the step created stays, and the next Producer or `clean` finishes or removes it" and cites new 3.2 step 7 for `start.io`.
- 2: keep. `tryAcquire` step 2 cuts "which `start` turns into a `start.io` fault and maintenance into a failure" (owners new 3.2 step 7; maintenance 2 and 6).
- 3.1: keep. 3.2: keep the reading rules and the corrupt and incompatible classes, and keep "the detail names the first field that differs: ..." as what the comparison reports. Cut "`start` turns corrupt into a `Fault` at `start.header` and incompatible into `Rejected` at `start.incompatible`" (owner new 3.2 step 7) and "Maintenance reads the header only to check it and digest it" (owner maintenance 2). 3.3: keep.
- 4 and 4.1 to 4.3: keep; link the record "Bodies are mapped, checksummed with CRC32C and versioned by random identifiers" at the commit identifier. 4.4: keep the definition and the shared implementation; the hardware paths shrink to one clause.
- 4.5: keep the opening with the definition of integrity, the table and the declarations. Cut the sentence that summarizes each reader's mode (owners container 7, 7.3, 8.2 and maintenance 2) and "A failure in an opened body is invalid material at the check's name" (owner SPEC-integrator.md new 6.2). Keep the `Integrity` layout sentence and the `BodyLayout` sentence.
- 5.1: keep. The roles sentence stays as the rule II21 cites. Keep "Nothing asks the index to hold every body on disk" with a link to the record "The scoring read opens by name", and the registry-holds-no-lock sentence. Link the record "One opened artifact per process".
- 5.2: keep. The build-failure sentence takes old 9's clause: the object is destroyed unregistered.
- 6.1: keep.
- 6.2: keep. Add the sentence that moves up from the history: a listing builds its map under `m_indexLock`, because a map built outside it would drop what a commit or an event recorded meanwhile. Link the record "Listings".
- 6.3: keep. The inotify read-error clause takes old 9's "the descriptor stays". The paragraph on a VM's own commits shrinks to a pointer to 8.2 step 8. Keep the paragraph on the kernel queueing a rename's events before the bump and on a crash between them, which II16 rests on. Link the record "Other processes' commits arrive through an epoch and inotify".
- 6.4: keep.
- 7 and 7.1 to 7.3: keep; link the mapping record at 7.2.
- 7.4: cut the first sentence (owner THREAD Session; container 4.5 holds the split of each check). Keep that neither mode defends against a writer of the artifact directory and the truncation consequence.
- 7.5: keep.
- 8.1: keep.
- 8.2: keep steps 1 to 8. The closing paragraph shrinks to two sentences: step 8 keeps the object's index current without a refresh, and an object that was behind refreshes at its next lookup. Keep "Any failure after step 3 unlinks the temporary, best effort, and returns `CommitFailure`". The sentence on what a killed process leaves cites harness 12 (owner). Cut "Commits are independent and process-crash-consistent" (owner THREAD Capture) and keep "no `fsync` is issued (THREAD Capture excludes power-loss durability)".
- 8.3: keep.
- 9 (Filesystem errors): cut. Owners: new 3.2 step 7 (the `start` rows), container 2 (`flock`), container 7 and 7.1 (`token`, `open`, `readSavedSummaries`), container 8.2 (the writer), container 6.2 and 6.3 (the listing and refresh rows). Its two clauses no section stated move to 5.2 and 6.3.
- 10 → 9 Tests: keep C1 to C8.

## 4. Harness sub-SPEC

- Preamble: keep; link the history; add that the file's native facts, N16 to N25, sit in the sections they serve, each read in the code at the pin except N17's measurement.
- 1: keep. 2: keep.
- 3: the heading stays, "Twin checks in a VM". Old 3.1 is cut. Owners: SPEC-integrator.md new 3.2 steps 3 and 10 and new 4.1's parts table (the twin budget and report), R-ALL-2 (twin checks run only while `twinReportSink()` is non-null), harness 3 itself (the `Twins` object lives in the image twin-check state) and new 4.6 (`willDestroyVM` destroys that state, then closes the report). Old 3.2's table and old 3.3's text follow the heading. In old 3.3, keep the declarations, the creation at the first stash with its deleter, the consumption in `didFinishPrepareForExecution` and the sentence showing that the `Twins` object's lifetime meets SPEC-image.md R-INT-11; cut "It holds no twin CB: each twin lives only for its check and dies without writing `didOptimize` ..., so when `willDestroyVM` destroys it matters to no twin" (owners SPEC-image.md section 17.2, step 2, and R-INT-11's item on the per-VM `Twins` object, which lets VM destruction destroy the object at any point because it holds no twin CB).
- 4: begins with N16 and N17 (N17 keeps its facts and the note that ARM64 is unmeasured; "this was measured, not read, as SPEC-integrator-history.md records" becomes a link to the record "Twins builds keep Bun's build flags, and the runner moves the heap"). The first paragraph's restatements of SPEC-image.md R-INT-11 and THREAD Verification's relocation skips shrink to citations; keep that placement moves two domains, randomization moves the heap, the runner calibrates, and H2 checks. Keep the declarations, the layout lines, the placeholder rule, both hosts' call points (taking from new 11.2 that Bun records the main VM's layout whether or not JITCache is configured) and the failure rule.
- 5.1, 5.2, 5.4: keep. 5.3: cut "In twins builds Bun takes `--jitcache-twins-report` as `Config::twinReportPath` and `--jitcache-body-events` as the path of the body-event dump" (owner SPEC-integrator.md new 11.2); keep the exports and the rest.
- 6 and 6.1 to 6.4: keep; link the record "The oracle compares the reachable heap" at 6.
- 7.1 to 7.3: keep; link "Directives address runs by sequence" at 7.2.
- 7.4: keep the table. The second paragraph cites THREAD Verification's skip rule, SPEC-image.md section 17.2's preconditions for when the image check skips, and R-INT-11's item that turns `useConcurrentJIT` off in every process of a twins run, instead of restating them (owners those passages); keep the runner's rule, kept as written (gap G4), and the sentence on the directories' defaults and runs that set concurrency. Link the record "When a skip fails a run".
- 7.5: keep; link "The runner fails undeclared faults and runs that install nothing". 7.6: keep; link "The oracle compares every run that configures JITCache". 7.7: keep as written. 7.8: keep.
- 8.1 to 8.3: keep; link "testjitcache applies options in the initialization callback" at 8.2.
- 9.1 to 9.3: keep, with 9.2's `request` row as written (gap G10); link "Measurement choices" at 9.1 and 9.3.
- 10: begins with N21 to N25. The introduction's "Tests assert these counts and benches read them as trends" goes (owner HARNESS.md, "The skipped warm-up"). 10.1 to 10.3: keep. 10.2 gains the sentence that moves up from the history: the function does not fall back to the UCB the function's `UnlinkedFunctionExecutable` holds, because those slots are private and the public accessor would decode, generate or import one. Link the record "Per-body event counts".
- 11: the introduction's first sentence cites HARNESS.md ("The off path emits the pin's code") and THREAD Capture instead of restating them; then N18 to N20; keep the method sentences. 11.1 to 11.5: keep; 11.2 cites N21. Link the record "The pin comparison names addresses itself".
- 12: keep, including "Bun-hosted runs have no equivalent (section 7.7)" as written (gap G2). Link the record "Producer kills".
- 13: keep H1 to H8.

## 5. Maintenance sub-SPEC

- Preamble: keep; link the history.
- 1, 3, 4.2 to 4.5, 6, 7: keep.
- 2: keep.
- 4.1: cut the first sentence (owner THREAD Maintenance). Keep the stamps, P as SPEC-cb.md section 4.3 computes it, and the formula.
- 5: keep the grammar, the outputs, the exit codes and the Bun wiring, which is the one place for `bun jitcache` (new 11.2 cites it). The jsc sentence keeps "the jsc shell runs it with `--jitcache-maintenance`" and cites new 11.1 for when (owner of the timing: new 11.1).

## 6. The history

The history becomes a set of decision records. Each states what first seemed right, the evidence that changed it and the intent that came out, or, for a choice that never changed, the alternative a later agent would reach for and why it lost; two to six sentences, with no round names, batch numbers, severities or dates. The SPEC links each record by its title where the decision sits. The history's binding-free status line stays.

### What moves up into the SPEC

Two reasons the SPEC lacks and an implementer could act against:

1. Container 6.2: a listing builds its map under `m_indexLock`; a map built outside it would drop what the writer's commit and the drained events recorded meanwhile (from "Kept minors", integrator batch 8.2).
2. Harness 10.2: `jitcacheBodyEvents` does not fall back to the UCB an `UnlinkedFunctionExecutable` holds, since those slots are private and the public accessor would decode, generate or import one (from "Native-fidelity review after the minors", finding 2).

Every other decision the history explains is already stated in the SPEC.

### Records the history keeps

| record | sources in the old history | linked from |
|---|---|---|
| The fault entry point names its site | Draft 1, "Fault entry points"; Round 1, "Other changes in round 1"; the drain's sentence on the pinned signature; kept minor 5.2 (release at the next glue entry) | 4.5 |
| Every refused production charge is `budget.limit` | kept minor 4.6 | 4.5 |
| The state is created and destroyed in task 7's file | Round 3, "The state's lifecycle moved to the task that creates states" | 4.1, 17 |
| Test bodies and a lookup override | Round 3, "Test bodies and a lookup override for the lanes' self-tests"; the drain's sentence on the override answering tokens | 6.2 |
| `productionActive` belongs to the lanes' interface | five-part round 2, "Production for the UCB lane's direct-eval contexts" | 4.2 |
| Strict: integrity in both modes, structure under strict | the drain's paragraph on THREAD's changes (the container's split, the writer's reread staying `Full`) | 4.3, container 4.5 |
| Which compilations record is decided at plan construction | kept minor 8.a | 4.4 |
| A ConsumerProducer rebuilds the image record only while production is active | kept minors 2.4, 3.8, 5.0, 7.10 | 7.2 step 7 |
| Body files are named by the key's hex | Draft 1, "File names" | container 1.1 |
| Bodies are mapped, checksummed with CRC32C and versioned by random identifiers | Draft 1, "Reading bodies"; kept minor 1.7 | container 4.1, 4.4, 7.2 |
| Other processes' commits arrive through an epoch and inotify | Draft 1, "Seeing other processes' commits", whose "Producers need neither" the next record supersedes; kept minor 7.8 (temporaries in `cache/`, one event per commit) | container 2, 6.3 |
| One opened artifact per process | Round 2, "Shared opened artifacts did not refresh by role", with the per-VM alternative and its costs | container 5.1 |
| Listings | Round 2, "Every listing after the first read an empty directory"; kept minor 8.2 (the fallback interval; the map built under the lock) | container 6.2, 6.3 |
| The index holds tokens | the drain's paragraph on UCB R-INT-3; kept minors 2.6 and 4.9 (erase only while the token is unchanged); kept minor 6.7 (compact index entries wait for IB2's measurement) | 6.2, container 6.1, 16 (IB2) |
| The scoring read opens by name | Round 1, "A transient open failure scored a saved body as absent"; Round 3, "The scoring read no longer needs a complete index"; kept minor 6.6 (a kept summary recorded at install was rejected as work ahead of demand; IB7 measures first) | 8.3, container 5.1, 7.3, 16 (IB7) |
| The writer takes section sources and reads no lane format | Round 1, "The writer parsed lane formats" (the writer's part); Round 3, "The writer takes section sources" | 6.3, container 8.2 |
| Charging hash tables and index entries | Draft 1, "Scoring and kept summaries" (the charge); Round 2, "The index was exempted from the producer limit" | 4.4 |
| One install function with a point argument | Draft 1, "The install function" | 7.1 |
| The kept score comes from the builders | Draft 1, "Scoring and kept summaries" (keeping the summary); Round 1, "The writer parsed lane formats" (the score's part); the drain's open finding (simplicity lens); kept minor 2.3; native-fidelity finding 5 | 8.3, 8.4 |
| `delta` scores in two phases | Draft 1, "Scoring and kept summaries" (second paragraph); kept minor 2.10 | 8.6 |
| Captures cannot nest | Round 3, "Captures cannot nest, so the reentrancy guard went" | 8.7 |
| Exit sites across ConsumerProducer generations | five-part round 1, "Exit sites induced across ConsumerProducer generations"; the drain's paragraph on IB10 | 16 (IB10) |
| The executable-allocation fault list is exhaustive | the drain's paragraph on P1 and the FTL FFI path | 9 |
| Bun | Draft 1, "Bun", both paragraphs, as written (gap G5) | 11.2 |
| Bun's twins build reaches only exported headers | five-part round 3, "Bun's twins build called functions only an unexported header declared" | 3.5, 11.2 |
| Twins builds keep Bun's build flags, and the runner moves the heap | Draft 1, "Test builds" (second paragraph); five-part round 1, "The heap relocation domain in the twins build"; the drain's paragraph on P2 | harness 1, 4, 7.3 |
| When a skip fails a run | Draft 1, "Test builds" (first paragraph); the drain's paragraph on P3; kept minor 8.3 (`ics/` turns concurrency off) | harness 7.4 |
| The oracle compares the reachable heap | Round 1, "The oracle had no way to compare the reachable heap" | harness 6 |
| The oracle compares every run that configures JITCache | kept minors 1.10 and 5.8 | harness 7.6 |
| The runner fails undeclared faults and runs that install nothing | walkthrough report "spec-integrator-runner-unexpected-faults" | harness 7.5, R-ALL-4 |
| Directives address runs by sequence | "Runner directives by sequence" | harness 7.2 |
| `testjitcache` applies options in the initialization callback | Round 1, "`testjitcache` set options before they existed" | harness 8.2 |
| Per-body event counts | "Harness pass for HARNESS.md", first paragraph; native-fidelity findings 1, 2 and 3 | harness 10 |
| The pin comparison names addresses itself | "Harness pass for HARNESS.md", second paragraph, with M3's last-member placement | harness 11, M3 |
| Producer kills | "Harness pass for HARNESS.md", third paragraph | harness 12 |
| Measurement choices | kept minors 1.5, 7.3, 7.4; native-fidelity finding 4 | harness 9.1, 9.3, 7.2 step 18 |
| What the executable-allocation fault tests can see | kept minor 1.2; walkthrough report "spec-integrator-exec-alloc-test-cannot-see-captured-effects" | 15.2 |
| Imports a process captured itself | five-part round 3, "Imports a process captured itself" | harness 4, 7.5 |
| The lanes' twin checks land before the glue that calls them | five-part round 2, "The lanes' twin checks waited on the glue that calls them" | 17 |
| The option table lives in options.md | "Options table" | 5.1 |

Order in the file: the first eight records (interface and per-VM state), then the nine on the artifact, the six on install and capture, the two on hosts, the fourteen on test builds and the harness, and the one on options.

### What the history drops

Logs and routine fixes, which git keeps:

- Draft 1's opening sentence, "Where the integrator meets each lane" (a mapping log; the conditional Image M8 is stated in new 14.5) and its paragraph on `status` running on the VM thread (stated in new 3.3 and 4.2).
- Round 1, "Bun's main VM has execution context id 1" (N15 states the derivation and that the comment is stale) and "Task 1 could not reach its own acceptance".
- Round 2, "The task plan scheduled tests and calls before the code they need" and "Other changes in round 2".
- Every round's count of findings and severities.
- The drain's provisional-mark bookkeeping, its list of lane passages left to the lanes (SPEC-ucb.md R-INT-4, SPEC-image.md section 15.4 and R-INT-11 now agree with this set) and its other routine changes.
- "Kept minors from the thread-prep run", except the entries merged into the records above. The dropped entries are routine fixes, each now stated in the SPEC, or entries marked "already fixed". This includes batches 3.6 and 8.c, which mapped run options to `BUN_JITCACHE_*` variables; they are logs, and the decision they served is the record "Bun" (gap G5).
- "Compaction (2026-10-07)", "Report drain", and "Walkthrough reports" except the two entries merged above.

## 7. Map

The data returned with this plan holds every entry below, with each section and each ID of a range as its own entry. A plain number names SPEC-integrator.md; its `new` field also says what happens to the same number in each sub-SPEC and, for old 3.2, 4.4, 5.1 to 5.4, 7.4, 8.4, 9.4, 9.5, 9.7, 10, 12 and 14.2, which sentences leave the section or join it, as section 2 lists them; old 10's entry also says that its FFI paragraph's closing clause cites THREAD Failures instead of old 5.4. Prefixed entries ("container 4.5") cover the sub-SPECs one by one. A rule ID's `new` is the ID itself, since IDs never change; the tables say where each one lives. M1 to M10 have plain entries, as the set's inventory lists them, and prefixed ones ("manifest entry M3", "maintenance test M3") that say which file holds each. Only five sub-SPEC numbers change: container 9 (cut), container 10 (becomes container 9), harness 3.1 (cut), and harness 3.2 and 3.3 (merged into harness 3). Because the workflow's check reads a prefixed entry as a rule ID, the data records container 10's renumbering as a merge into container 9, the slot old 9 leaves.

### SPEC-integrator.md sections

| old | new | action |
|---|---|---|
| 1 | 1 | keep |
| 2 | 2 (N16 to N25 leave for the harness) | keep |
| 3 | 14 | move |
| 3.1 | 14.1 | move |
| 3.2 | 14.2 | move |
| 3.3 | 14.3 | move |
| 4 | 3 | move |
| 4.1 to 4.5 | 3.1 to 3.5 | move |
| 5 | 4 | move |
| 5.1 | 4.1 | move |
| 5.2 | 4.2 | move |
| 5.3 | 4.4 | move |
| 5.4 | 4.5 | move |
| 5.5 | 4.6 | move |
| 6 | 5 | move |
| 6.1 | 5.1 | merge |
| 6.2 | 5.1 | merge |
| 6.3 | 5.1 | move |
| 6.4 | 5.2 | move |
| 7 | 6 | move |
| 7.1 to 7.3 | 6.1 to 6.3 | move |
| 7.4 | 12 | move |
| 8 | 7 | move |
| 8.1 to 8.4 | 7.1 to 7.4 | move |
| 9 | 8 | move |
| 9.1 to 9.7 | 8.1 to 8.7 | move |
| 10 | 9 | move |
| 11 | 4.3 | merge |
| 11.1 | cut: each row's rule lives at its step (new 3.2, 3.4, 4.5, 6.2, 7.2, 7.3, 8.2 to 8.6, 9; container 8.2); two rules no step stated move to new 8.4 and 4.4; its closing sentences go to THREAD Failures and new 4.2 | cut |
| 11.2 | 4.3 | move |
| 12 | 10 | move |
| 13 | 13 | keep |
| 14 | 11 | move |
| 14.1, 14.2 | 11.1, 11.2 | move |
| 15 | 14 | merge |
| 15.1 | 14.4 | move |
| 15.2 | 14.5 | move |
| 16 | 15 | move |
| 16.1 to 16.3 | 15.1 to 15.3 | move |
| 17 | 16 | move |
| 18 | 17 | move |

### Sub-SPEC sections

| old | new | action |
|---|---|---|
| container 1 to container 8.3, every section | same number | keep |
| container 9 | cut: new 3.2 step 7, container 2, 6.2, 6.3, 7, 7.1, 8.2; two clauses move to container 5.2 and 6.3 | cut |
| container 10 | container 9 | renumbered (recorded as merge) |
| harness 1, 2, 3 | same number | keep |
| harness 3.1 | cut: new 3.2 steps 3 and 10, new 4.1, R-ALL-2, harness 3, new 4.6 | cut |
| harness 3.2, 3.3 | harness 3 | merge |
| harness 4 to harness 13, every section | same number | keep |
| maintenance, every section | same number | keep |

Plain numbers that only sub-SPECs define (1.1, 1.2, 1.3, 7.5, 7.6, 7.7, 7.8, 10.1, 10.2, 10.3, 11.3, 11.4, 11.5) keep their numbers.

### Rule IDs

| IDs | new home | action |
|---|---|---|
| N1 to N15 | section 2 | keep |
| N16, N17 | harness 4 | move |
| N18, N19, N20 | harness 11 | move |
| N21 to N25 | harness 10 | move |
| R-ALL-1 to R-ALL-3, R-ALL-5 to R-ALL-8 | section 12 | move |
| R-ALL-4 | section 12, holding the `Off`-role convention the ucb plan's X1 moves in (section 2, new 12) | move |
| II1 to II23 | section 13 | keep |
| M1 to M10 (manifest entries) | 14.4; maintenance tests M1 to M6 stay in maintenance 7 | move |
| T-OPT, T-CPU, T-BUILDID, T-BUDGET, T-BODY, T-FAULTS, T-START, T-LOOKUP, T-SCORE, T-STAMP, T-DELTA, T-CHARGE | 15.1 | move |
| the JS tests of old 16.2, by file name | 15.2 | move |
| IB1 to IB11 | section 16 | move |
| tasks 0 to 17 | section 17, same numbers | move |
| B1 to B8 | container 4.5 | keep |
| C1 to C8 | container 9 | move |
| H1 to H8 | harness 13 | keep |

### Citations other documents make into this set

For the references pass: options.md cites 4.2 (→ 3.2), 8.1 (→ 7.1) and 16.2 (→ 15.2); HARNESS.md cites 6.4 (→ 5.2), 16.2 (→ 15.2) and 17 (→ 16); SPEC-ics.md R-INT-6 cites 11.1 for the step `exec-alloc.ic-handler` (→ 4.5); SPEC-image.md cites 5.1 (→ 4.1), 5.4 (→ 4.5), 7.1 (→ 6.1) and 9.1 (→ 8.1); SPEC-ucb.md cites 5.1 (→ 4.1) and 5.2 (→ 4.2). Citations of harness 5.2, 5.4, 7, 7.2, 7.4, 7.6, 7.7, 9.1, 9.2 and 11.2, of container 1.1 and 4.5, and of IDs, keep their targets.

## 8. Cross-set cuts and moves

Cuts whose owner is in another set:

| what is cut | owner |
|---|---|
| Old 8.2 step 15 (new 7.2): what `finishCounter` does (re-slice a carried counter against the consumer's pool at least two entry increments short of crossing, resample aging, leave setup's arming for a counter that does not travel) | SPEC-cb.md section 5.3 |
| Old 9.1 item 2 (new 8.1): when an image record is `Complete` (shareable code a recording compilation emitted while cache activity was on, so without a PC-to-origin map) | SPEC-image.md section 10 (`isImageCapturable`) and section 4.1 |
| Old 5.3 (new 4.4): the semantics of `jitCacheRecordsImage` (written once on the VM thread before the plan is enqueued, read without a lock; one null test without a state; a UCB without a key at compile time never gets one; the twin's plan computes it the same way and its recorder ignores it) | SPEC-image.md R-INT-12 and section 4.1 |
| Old 5.4's site table (new 4.5): the names of `InlineCacheCompiler`'s three `didFailToAllocate()` branches | SPEC-ics.md E5 |
| Old M1 (new 14.4): the UCB lane's ten `Sources.txt` files | SPEC-ucb.md M1 |
| Old M1: the Image lane's ten `Sources.txt` files | SPEC-image.md M1 |
| Old M1: the CB lane's six `Sources.txt` files | SPEC-cb.md M2 |
| Old M1: the ICs lane's four `Sources.txt` files | SPEC-ics.md M1 |
| Old IB10 (new 16): that the ICs lane sets the no-progress bit for a call site the producer left polymorphic | SPEC-ics.md section 5.3 |
| Harness old 3.3 (new harness 3): why the `Twins` object's destruction time matters to no twin | SPEC-image.md section 17.2 (step 2 and the `Twins` declaration) and R-INT-11's item on the per-VM `Twins` object |
| Harness 7.4: when the image check skips, and that twins runs turn concurrent JIT off in every process | SPEC-image.md section 17.2 (`checkImage`'s preconditions) and R-INT-11's item that turns `useConcurrentJIT` off in every process of a twins run |

Restatements of THREAD, options.md and HARNESS.md that this plan cuts are listed in sections 2 to 5 with their owners. None of those documents is rewritten in this pass.

Moves into another set: none.

Moves into this set: the ucb plan's X1, the `Off`-role convention of SPEC-ucb.md old 13.3's third bullet, which R-ALL-4 takes (section 2, new 12). SPEC-image.md old 17.1, SPEC-cb.md old 11.3 and SPEC-ics.md old 11.2 state the same rule for their own scripts, so their plans can replace their copies with a citation of R-ALL-4. Each lane keeps the clauses that R-ALL-4 leaves out:

| set | what R-ALL-4 holds | what stays with the lane |
|---|---|---|
| ucb, old 13.3 | the bullet, except one clause | "without statistics" (`$vm.jitCacheUCBStatistics()`) |
| image, old 17.1 | the whole sentence; its "the same program" is the Consumer path R-ALL-4 names | nothing |
| cb, old 11.3 | the rule | "without reading or writing `scratch`" |
| ics, old 11.2 | the rule | "without `saveJSON` or `loadJSON`"; T2's sequences pass `Off` too; T2's assertions as its example of native ones |

Until a lane's plan makes that change, its copy restates R-ALL-4 and loses nothing.

## 9. Contradictions and gaps, reported and left unfixed

- G1. New 3.2 (old 4.2) opens with "`start(vm, config)` runs on the VM thread before the VM's first global object (THREAD Session)". THREAD Session allows a later call, which misses the bodies created before it, and no step of `start` rejects one, so the sentence reads as a precondition THREAD does not impose.
- G2. Harness 12 ends "Bun-hosted runs have no equivalent (section 7.7)", while new 11.2 has Bun declare every test flag of harness 5.1, `--jitcache-test-kill` included, with the effect it has in the shell, and harness 7.7 passes every `--jitcache` run option through to Bun.
- G3. Bun's test flags need functions no exported header declares. New 11.2 gives each test flag of harness 5.1 its shell effect, and task 14 builds Bun against the copied headers only (new 3.5). `JITCacheTwinsHost.h` declares the placement and layout functions, the five `bun:jsc` host functions, `writeBodyEvents` and `setImageTestHookNamed`, and nothing exported reaches the store's fault hook (`StoreTesting::setFault`, `ArtifactStore.h`) for `--jitcache-test-store-fault`, the writer's fault and kill hooks for `--jitcache-test-writer-fault` and `--jitcache-test-kill`, `setForcesBlindingForTesting` (which new 14.1 says only the jsc shell calls) for `--jitcache-test-force-blinding`, the twin report for `--jitcache-test-twin-entry`, or `describeReachableHeap` for `--jitcache-describe-heap`. Where Bun would write the end-of-run heap description, relative to `delta` and the body-event dump, is unspecified, and harness 5.3 notes that Bun's global object holds per-process state, which the default roots would describe.
- G4. Harness 7.4 fails a run on a skip exactly when that run and every earlier JITCache run of its sequence have `useConcurrentJIT` off, "since those are the importing process and the processes whose captures it imports". An earlier Consumer run configures JITCache and captures nothing; when it alone runs concurrently, THREAD Verification fails the skip and the runner lists it. Minor.
- G5. The history has no record of the switch from `BUN_JITCACHE_*` variables to Bun taking the shell's flags (THREAD Session). The user's request for that change said to leave the history files alone, so the record "Bun" keeps Draft 1's text as written and this plan adds nothing to it. Whether to add the evidence and the intent is for the human.
- G6. Cross-set: SPEC-image.md R-INT-11 still sets the image test hook through "the matching Bun environment variable", while Bun now takes `--jitcache-test-image-hook` as a flag (new 11.2, new 14.3).
- G7. Outside the sets: README.md's Getting started runs a producer without `--jitcache-producer-limit`, which `start` rejects at `start.config` until the bench sets `defaultProducerLimitBytes` (new 3.2 step 3; the parameter table of new 16).
- G8. N15's last bullet, that Bun bundles its `.cpp` files into unified sources, supports no rule: nothing says how `JITCacheHost.cpp` names its file-local helpers. Minor.
- G9. `delta`'s candidate table charges each candidate "for the vector slot it takes" (new 8.6 step 3). That does not cover a `Vector`'s capacity beyond its size, while II4 requires every production byte to be charged before its allocation. Minor.
- G10. Harness 9.1 and 9.2 disagree on the clock that times each part of the `request` event. 9.1 has a breakdown of a counted span into steps read `CLOCK_MONOTONIC`, as wall time that only explains the total, while 9.2's `request` row records "the CPU time of each part B2 lists". SPEC-ucb.md B2, which lists the parts, times them by 9.1's rule, and this plan keeps both harness passages as written, so 9.2's row still names CPU time. Minor, since the rule and the lane that records the event agree.
