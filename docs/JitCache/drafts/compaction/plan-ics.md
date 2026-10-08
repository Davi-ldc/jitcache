# Compaction plan: the ics set

Files: `docs/JitCache/specs/SPEC-ics.md` and `docs/JitCache/specs/SPEC-ics-history.md`. The pass changes form, never decisions. Section numbers 1 to 14 keep their meaning; only the subsections of section 6 and of section 10 change. No SPEC outside the set, and neither THREAD, HARNESS.md nor options.md, cites a section number in 6 or 10: they cite sections 4, 4.2, 5, 5.2, 5.3, 5.6, 11.1 and 13, and rule IDs. Two history records do: SPEC-cb-history.md cites section 6.3 and SPEC-integrator-history.md cites section 10.2, which the references pass retargets through the map to 6.1 and 10.

Section numbers in other sets are their current ones, which the references pass retargets. For SPEC-integrator.md, whose plan renumbers most sections, the new number follows in parentheses; that plan also cuts old section 11.1, so no owner named here is 11.1.

## 1. Reading order of the rewritten SPEC

1. Header: lane 4 of THREAD Execution, binding, the lane's whole design, the history link, the namespace and the `ENABLE(JITCACHE_TWINS)` guard.
2. Section 1, scope: what the lane owns, each item citing the THREAD section that assigns it; what lies outside; THREAD's omissions.
3. Section 2, design in brief: orientation only, about three sentences that point to the sections holding the rules and restate none of them.
4. Section 3, census: every field of the two native objects and its fate: restored through the derivation (section 6.1), left as installation or linking leaves it, or never carried, each with its THREAD source.
5. Section 4, the section format: layout, orders (now with the reason for dense records), records (producer facts only), types, the section's checks (the lane's one statement of normal mode against strict), site walkers.
6. Section 5, capture: where it runs (by citation), classifying a property IC and the four native states, the summary and the polymorphic bit (purpose, rule, then the evidence THREAD asks for), call links, capture's checks, interface.
7. Section 6, restoration: the three calls and the CB state each finds; 6.1 the derivation; 6.2 prepare, with S1, S2 and its declarations; 6.3 seeding; 6.4 attaching; 6.5 postconditions.
8. Sections 7 to 14 in their current order. L4 in section 9 also holds why a capture sees a consistent state. Section 10 holds only the options pointer.

The census decides what travels, the format encodes it, capture fills it and restoration applies it. The one reorder puts the derivation first in section 6: seeding and attach apply its functions, S1's reason is its round trip, and I3 to I5 are stated in its terms, so a reader of the old order had to jump ahead two subsections and come back.

## 2. Passages

### Header

- Keep lane 4, binding, the namespace and the guard. Replace "holds rationale and review records" with what the new history holds: the path to each decision that is not obvious. Drop "the lane has no sub-SPECs"; "this file is the lane's whole design" already says it.

### Section 1

- Merge the first paragraph's paraphrase of THREAD's assignments into the numbered list, so each owned item appears once with its THREAD citation (Execution, Capture, Restoration, Caches). Keep the clause on DFG and FTL: it is why the lane has one section per tier (section 4).
- Keep "Outside the lane" and the omissions paragraph.

### Section 2

- Keep the heading so later numbers hold. Rewrite it as orientation: one record per property IC and per call-link site, raw producer facts in native order (4.2, 4.3); one pure derivation per record kind, reused by the twin check, and the recapture function with I5 (6.1); the polymorphic bit (5.3). About 70 words in place of 230.
- Move "so restore walks them beside the CB's ICs and metadata entries with no lookup" to 4.2, as the reason for dense records.
- Move "whether the body has a site whose cases or callees an early DFG compile would see only in part" to 5.3, as the bit's purpose.
- Cut the rest as restatement inside the set: the learning-group list (3.1's group column), the four facts (5.2), raw facts and derivation (4.3, 6.1), the call-link rule (3.2, 6.1).

### Section 3

- Keep the definitions of "installation" and "linking", the three tables and the FTL note.
- Rows whose value is a formula of the derivation say "restored (section 6.1)" with their THREAD source: `canBeMegamorphic`, `m_slowOperation`, the whole learning group; `m_mode`, `m_hasSeenShouldRepatch`, the three history bits and the varargs maximum. 6.1's tables become the one statement of each value; I3 and I4 already speak in terms of the derivation.
- Rows for fields that do not travel stay as they are.

### Section 4

- Intro: keep one section per tier and no tier field; cite R-INT-1 for the type id instead of restating it.
- 4.1: keep.
- 4.2: add the dense-order reason from section 2. Keep the null-table facts and compress their evidence to one sentence: generation sets `m_hasMetadata` only in `addEntry` and `addValueProfile`, a decoded table takes it from its constructors (`UnlinkedMetadataTable::empty` for a table encoded without metadata), and `finalize` clears it on offset overflow. Keep both examples and the consequences (no group, no record, metadata walked only through 4.6). Note that mold order meets SPEC-image.md R-ICS-1. Keep the build-ID sentence. SPEC-cb.md cites this section for the null-table example, so the example stays here.
- 4.3: keep; link the history beside "The record holds producer facts only".
- 4.4: keep the code block byte for byte.
- 4.5: becomes the lane's one statement of normal mode: normal mode trusts what the checks verify, because THREAD Session has it check only integrity, which the integrator does first; strict runs them; debug builds assert A1 to A6. Keep A1 to A8, the pairing paragraph and the paragraph on testing the checks with plain data. 5.5 and 6.2 cite it.
- 4.6: keep.

### Section 5

- 5.1: cut the two-row table of capture points as a restatement of SPEC-integrator.md sections 9.1, 9.5, 9.6 and 9.7 (new 8.1, 8.5, 8.6 and 8.7). One sentence replaces it: the integrator calls the lane at THREAD's two capture points, in the context of those sections, on an eligible CB whose `BaselineJITData` is published, with no `CodeBlock::m_lock` held (L2); the lane adds no hook. Keep the paragraph on what capture functions allocate and call, citing L1 for the lock instead of restating `ConcurrentJSLocker`. Move the consistency paragraph into L4.
- 5.2: keep items 1 to 5 and states (a) to (d). Compress item 3's evidence to its conclusion: only the `PromoteToMegamorphic` arms install a `*Megamorphic` operation and both ways out of it empty the chain, so `megamorphicCaseListed` already says what it would; any operation other than the access type's `*GaveUp` restores as a reset leaves the IC. Compress item 5 to the bit's writers, its one reader and the conclusion. Link the history.
- 5.3: order the parts as (1) `icSitesWithCases` and why only property ICs count; (2) the bit's purpose (from section 2) and its two conditions; (3) the call-site evidence THREAD Restoration asks the SPEC to show, every causal step kept; (4) the call sites that stay out; (5) the bit reads the captured record and is no restored state, so a recapture can drop it (B6). Cut the closing account of how THREAD Capture ranks a withheld counter, which SPEC-cb.md section 4.3 holds; cite it and THREAD Capture. Link the history twice (summary, bit).
- 5.4: keep.
- 5.5: keep the two call checks, C1 and "Capture checks nothing else"; cite 4.5 for normal mode and keep "debug builds assert them". Link the history.
- 5.6: keep the code and both paragraphs.

### Section 6

- Intro: keep the call table with the columns call, CB state on entry and lock the lane takes; the position column becomes "R-INT-4", which holds the positions. Cut the paragraph on the deferrals and the unpublished CB as a restatement of SPEC-integrator.md section 8.4 (new 7.4), leaving one sentence: no other thread reads what the lane writes before `installCode` (SPEC-integrator.md section 8.4), and attach still writes under the lock (L3). Keep the sentence that the seeds land only in the installing CB, which T8 tests.
- 6.1 Derivation (old 6.3): keep the code block, both tables and the prose on folded, given-up and beside-cases sites. Link the history beside the given-up rule and beside the zero wait counter.
- 6.2 Prepare (old 6.1 and 6.2 merged), under the heading that names `jitcache/ICRestore.h`: prose first (what `preparedCode` is, I1, the payload kept alive per R-INT-4, normal mode by citation of 4.5, the strict order, S1, S2), then the code block, then "`PreparedBaselineICs` owns nothing". S1 now cites the round trip of 6.1, which precedes it. Link the history beside the choice of `PreparedImage::code()`.
- 6.3 Seeding (old 6.4): keep.
- 6.4 Attaching (old 6.5): keep the pseudo-code. Compress the paragraph on what the status classes read after attach to its cases: a considered IC reads like a native IC after a reset (`Simple`, and the generic put THREAD Caches names), a site that gave up beside its cases (`Simple` or `TakesSlowPath` by its captured bit), a never-considered IC (`NoInformation`), a given-up IC (`TakesSlowPath`). Note that attach indexes ICs by mold index (SPEC-image.md R-ICS-1). Link the history beside "the shape group and the handler chain stay as installation built them".
- 6.5 Postconditions (old 6.6): keep I1 to I5.

### Section 7

- Intro: keep.
- E1: keep the code. Compress the description of the current branch and the LLInt's two handlers to two sentences. Keep "Both registers are free here", the no-fixup sentence (it meets SPEC-image.md R-ALL-1; cite it), the barrier sentence (link the history) and the native-change paragraph. In the last paragraph keep the thread and lock context and "E1 touches only this branch"; cut "No other part edits `JIT::compileOpCall` ... so the lane applies E1 itself" as a restatement of SPEC-image.sites.md section E, and cite that section.
- E2, E3, E4: keep; the E3 table stays byte for byte. Link the history at E3 and E4.
- E5: compress the opening (THREAD assigns the sites; the integrator owns the entry point, R-INT-6) to one sentence; keep the three sites, the call and its context.

### Section 8

- 8.1: keep; update "declared in sections" to 4.4, 5.6, 6.1, 6.2, 7 and 11.1.
- R-INT-1: keep.
- R-INT-2: keep the call sequence, the `CaptureError` mapping, the order before the CB lane and the bit it passes. Cut "a body captured with the bit carries no baseline counter progress, so its envelope's P is zero", held by SPEC-cb.md I16 and SPEC-integrator.md section 9.4 step 7 (new 8.4). Replace the lock sentence with a citation of L2. The integrator still cites R-INT-2 for the bit, which stays.
- R-INT-3: keep where the candidate's count and bit come from, the saved body's reader and that its `Invalid` is invalid material. Cut the scoring consequence and the tie-break order, held by SPEC-cb.md section 4.3 and its R-INT-5 and by SPEC-integrator.md section 9.2 (new 8.2), whose `beats` realizes THREAD Capture's order.
- R-INT-4: keep; SPEC-ucb.md cites its last sentence.
- R-INT-5: keep.
- R-INT-6: keep the entry point, the site and what E5's context needs: callable on the VM thread with `CodeBlock::m_lock` held through `GCSafeConcurrentJSLocker`, from any tier's IC compilation and in any VM, a no-op for a VM `start` never configured, failing step `exec-alloc.ic-handler`. Cut the rest of the behavior list, held by SPEC-integrator.md section 5.4 (new 4.5). Its citation of section 11.1 for the step becomes one of section 5.4, whose site table names the step, because the integrator set cuts 11.1.
- R-INT-7: keep items 1, 2 and 4. Item 3 names `jitcacheDelta()` and `jitcacheStatus()`, test builds only, with the behavior SPEC-integrator.harness.md section 5.2 gives them; its sentence on why the shell may call `delta` moves to the history.
- R-INT-8: keep.
- R-INT-9: keep the requirement: a runner for the C++ tests under `Source/JavaScriptCore/jitcache/tests/` that calls `JSC::initialize()` once before any test, because a test's VM needs the process-wide setup it performs, and gives each test that needs a VM a fresh one. Keep the phrase the harness quotes ("created after `JSC::initialize()` with default options and without a `start` call, with its API lock held"), "tests without a VM, T7's included, read no option and need nothing from it", and the closing sentence that the live tests build their own objects, so the runner needs no lane-specific hook. Cut the description of the framework (a test registered by name, the needs-VM flag, the failure macro), held by SPEC-integrator.harness.md section 8.1. Cut the parenthetical list of what `JSC::initialize` sets up (`Options::initialize`, `ExecutableAllocator::initialize`, the Structure address space and `LLInt::initialize`) as no requirement: it only details the reason the kept clause states, and no rule rests on it. Harness section 8.2 makes the call without that list, so the list has no owner.
- R-IMG-1: one sentence naming `PreparedImage::code()` and what prepare relies on: the code setup will install, readable before `commit`, with the producer's molds in mold order. Cite SPEC-image.md sections 11.1 and 12.1 for the rest of its contract.
- R-UCB-1: keep; cite SPEC-ucb.md section 10.1, which meets it.
- R-CB-1: keep; cite SPEC-cb.md section 6.4, the other side of the same rule.
- R-CB-2: keep the requirement (capture and `scoreLive` take the bit; with it set they carry no progress and mark the counter withheld); cut the restated detail of the two sections, held by SPEC-cb.md I16 and sections 3.3 and 4.3.

### Section 9

- L1: keep; it is the one statement of capture's lock.
- L2: keep; SPEC-integrator.md section 12 cites it as the owner of the lock-order argument. Tighten wording only.
- L3: keep.
- L4: absorb 5.1's consistency paragraph: every writer of what capture reads (the learning fields, `tookSlowPath`, `m_slowOperation`, the clearing watchpoint's reset, the stub's replacement, unlinking and freeing) runs on the VM's thread while JS runs or in a stopped-world GC phase; JS is paused at both capture points and no such phase can start, so none runs during a capture; the lock orders capture only against readers on other threads. Keep the rest of L4. Link the history.
- L5: keep.

### Section 10

- 10.1: cut. Every row restates a rule held elsewhere.
  - THREAD and the set hold the lane's own rows. The opening sentence restates THREAD Failures. Capture's call checks and C1 raise recording faults (5.5), and THREAD Failures cancels the uncommitted body. `readBaselineICsSummary` and prepare fail as invalid material (4.5, 6.2, R-INT-3, R-INT-4). Seeding and attach cannot fail (I2). The IC allocation fault comes before the give-up is written (E5), and the harness reports a twin mismatch (R-INT-7 item 1). The closing sentence repeats 4.5's statement of normal mode, with 5.5's for capture.
  - The ICs section is charged at capture: R-INT-2 keeps the charge, SPEC-integrator.md section 9.4 step 4 (new 8.4) raises `budget.limit` when it is refused, and THREAD Failures makes that a recording fault.
  - A failed prepare destroys the prepared image without `commit` (R-INT-4; SPEC-integrator.md section 8.2 step 9, new 7.2) and writes nothing to the CB, which stays native (I1; section 8.2's closing paragraph and the CB column of section 8.3's `Abandoned` row, new 7.2 and 7.3). The pending import is left to die with its UCB (section 8.3's `Abandoned` row, new 7.3; THREAD Restoration).
  - Old SPEC-integrator.md section 11.1 lists these rows too, but the integrator set cuts it.
- 10.2 becomes section 10, retitled "Options". Link the history.

### Section 11

- Intro: keep. Link the history beside "never skips itself".
- 11.1: keep.
- 11.2 opening and conventions: keep the conventions on `resources/ics.js`, `noDFG` and `noInline`, `skip`, the anchor, keep-alive and `--useConcurrentGC=false`.
- Replace the three conventions that SPEC-integrator.md R-ALL-4 holds for every part's scripts (printing, the `main(...)` form, the `Off` role) with one bullet that cites R-ALL-4 and SPEC-integrator.harness.md sections 7.2, 7.3 and 7.6 and keeps only the lane's clauses:
  - `resources/ics.js` keeps no role-dependent state at its top level;
  - under `Off`, which T2's sequences pass as well as the oracle's runs, a script also leaves out `saveJSON` and `loadJSON`;
  - T2's assertions read only native state (the `super_construct` cache and the JIT type), so they are native ones, which R-ALL-4 runs in every role.
- R-ALL-4 holds the `Off` rule (the Consumer path without `delta`, no assertion about the state JITCache imported, seeded, attached or captured, native assertions in every role) through the sentence the integrator plan adds to it from the ucb plan's X1 (integrator plan, section 2 at new section 12, and section 8). Today each lane states that rule for its own scripts (SPEC-ucb.md section 13.3, from which X1 moves it), and R-ALL-4 does not. If R-ALL-4 lacks that sentence after the rewrite, the `Off` bullet comes back whole.
- The anchor paragraph: keep its chain of facts and compress the wording: the finalize capture counts no IC site with cases; a `delta` capture replaces it only by scoring higher; `noDFG` makes the body `CannotCompile`, so its counter moves only on `op_enter`'s slow path and richness alone could tie; an anchored capture wins on the IC count at the latest because nothing richness counts shrinks without overlapping drains; the plan's drain can race a marker's, which `--useConcurrentGC=false` prevents; after the plan no other drainer reaches the body. Every cited symbol stays. Link the history.
- The live C++ recipe: keep (SPEC-cb.md section 11.2 builds its live CBs the same way and cites it).
- T6: keep the ID; its text becomes the oracle comparisons of SPEC-integrator.harness.md section 7.6 over every sequence in `JSTests/jitcache/ics/`, which names T6 as its owner.
- T9: cut the sentence on what `jitcache-require-fault` does (SPEC-integrator.harness.md section 7.2). Add, from the history, that T9 relies on the directory's default of concurrent JIT off (SPEC-integrator.harness.md section 7.4) because the fuzzer counts every thread's allocations.
- Other tests: tighten wording only. Every driven state, option, directive and assertion stays.

### Sections 12 to 14

- 12: keep B1 to B6. The intro's second sentence cites THREAD Verification and 4.5.
- 13: keep the table and M1 to M6; update the section numbers in the `ICSection` and `ICRestore` rows. Replace the prefix sentence with a citation of SPEC-integrator.md R-ALL-8 naming the prefix `ics`. M5 cites R-INT-2 to R-INT-4 instead of restating their order.
- 14: keep tasks 1 to 9. Task 3 names the derivation as section 6.1; task 5 reads "sections 6.2 to 6.4, applying 6.1".

### Throughout

- Update citations inside the set through the map (`section 6.3` becomes 6.1, 6.1 and 6.2 become 6.2, 6.4 to 6.6 become 6.3 to 6.5, 10.2 becomes 10).
- Drop a file path beside a qualified symbol that a grep finds alone; keep paths in the E headings, in section 13, for file-static symbols and for offlineasm files.
- Code blocks, the E3 table, enumerator lists and test file names stay byte for byte.
- Prose follows the humanizer skill.

## 3. Cuts that another set holds

| passage cut | owner that holds it | what stays in SPEC-ics.md |
|---|---|---|
| 5.1, the table of the two capture points | SPEC-integrator.md sections 9.1, 9.5, 9.6, 9.7 (new 8.1, 8.5, 8.6, 8.7) | one sentence citing them; L2 |
| section 6 intro, the paragraph on deferrals and the unpublished CB, and the thread column | SPEC-integrator.md section 8.4 (new 7.4) | one sentence citing it; L3 |
| R-INT-2, "a body captured with the bit carries no baseline counter progress, so its envelope's P is zero" | SPEC-cb.md I16; SPEC-integrator.md section 9.4 step 7 (new 8.4) | the bit passed to the CB lane |
| R-INT-3, the scoring consequence of the bit and THREAD Capture's tie-break order | SPEC-cb.md section 4.3 and R-INT-5; SPEC-integrator.md section 9.2 (new 8.2), with THREAD Capture | the lane's two readers and their failure class |
| R-INT-6, the entry point's behavior list | SPEC-integrator.md section 5.4 (new 4.5) | the call context E5 needs and the step name, cited to section 5.4 in place of the cut section 11.1 |
| R-INT-7 item 3, the behavior of `jitcacheDelta()` and `jitcacheStatus()` | SPEC-integrator.harness.md section 5.2 | the two names, test builds only |
| R-INT-9, the framework's description: a test registered by name, the needs-VM flag, the failure macro | SPEC-integrator.harness.md section 8.1 | the requirement, its one-clause reason and the phrase section 8.2 quotes |
| R-IMG-1, the restated contract of `PreparedImage::code()` | SPEC-image.md sections 11.1 and 12.1 | one sentence naming what prepare relies on |
| R-CB-2, the detail of what the CB lane writes for the bit | SPEC-cb.md I16, sections 3.3 and 4.3 | the requirement |
| 5.3, the closing account of how a withheld counter ranks | SPEC-cb.md section 4.3 (with THREAD Capture) | the fact that a recapture can drop the bit |
| E1, "No other part edits `JIT::compileOpCall` ... the lane applies E1 itself" | SPEC-image.sites.md section E | "E1 touches only this branch" |
| 10.1, the row on charging the section: a refused charge is a recording fault | SPEC-integrator.md section 9.4 step 4 (new 8.4), with THREAD Failures | the charge in R-INT-2 |
| 10.1, the effects of a failed prepare: the prepared image destroyed without `commit`, the CB native with nothing written, the pending import dying with its UCB | SPEC-integrator.md section 8.2 step 9 and its closing paragraph (new 7.2), and section 8.3's `Abandoned` row (new 7.3), with THREAD Restoration | R-INT-4 (destroy without `commit`), I1 (prepare writes nothing); the lane's other rows are held by 4.5, 5.5, 6.2, I2, E5 and R-INT-7 |
| 11.2, scripts print only values that do not depend on the role | SPEC-integrator.md R-ALL-4 | a citation |
| 11.2, the `main(role, scratch, artifact)` form and `jitcache-heap: off` | SPEC-integrator.md R-ALL-4; SPEC-integrator.harness.md sections 7.2, 7.3, 7.6 | the clause on `resources/ics.js` |
| 11.2, the `Off` rule: a script runs its Consumer path without `delta` and without any assertion about imported or captured state, and assertions about native state run in every role | SPEC-integrator.md R-ALL-4, through the sentence the integrator plan adds from the ucb plan's X1 (integrator plan, section 2 at new section 12, and section 8) | "without `saveJSON` or `loadJSON`"; T2's sequences pass `Off` too; T2's assertions as the example of native ones |
| T6, the description of the oracle | SPEC-integrator.harness.md section 7.6 | the ID and a citation |
| T9, what `jitcache-require-fault` does | SPEC-integrator.harness.md section 7.2 | the directive itself |
| 13, the `ics` prefix rule for file-local helpers | SPEC-integrator.md R-ALL-8 | a citation naming the prefix |

Cuts held by THREAD rather than a set (5.3's tie-break, 12's "the bench measures the default") are cited, not listed here. One cut names no owner because it holds no requirement: R-INT-9's parenthetical list of what `JSC::initialize` sets up, the detail of a reason R-INT-9 keeps in one clause.

The `Off` row is the one cut whose owner gains the rule in this pass. R-ALL-4 already binds every script to the oracle, which compares each JITCache run with an `Off` run where `delta` fails with `delta.unconfigured` and nothing installs, so the sentence R-ALL-4 gains changes no decision. The cut rests on that sentence, and section 2, under Section 11, says what happens if it does not land.

## 4. Moves into other sets

None. Every requirement the set holds stays in it; where another set states a rule, the set cites it. The `Off` rule is no move of this set: the ucb plan moves its own copy into R-ALL-4 as X1, and 11.2 then cites R-ALL-4 (section 3).

## 5. History

### What moves up into the SPEC

- T9's dependence on concurrent JIT off, from the native-fidelity review's cross-set item: the fuzzer counts every thread's allocations, so a fuzz index means one allocation only without worker compilation. The SPEC states no such condition today, and a later edit that turned concurrency on in T9's runs would make it flaky.

### What moves down from the SPEC

- R-INT-7 item 3's reason that the jsc shell is the host in test builds, so `delta` stays a C++ entry point. The SPEC keeps "test builds only", which already decides it.

### The new history

One short header: what the file is for (the path to each decision that is not obvious: what first seemed right, what changed it, the rule that came out), that nothing in it binds, and that the SPEC wins. Then one entry per decision, in the SPEC's order, each headed by the decision in sentence case, written as prose without revision labels, dates, THREAD hashes or finding tallies. The SPEC links each entry beside the decision it explains (the places are named in section 2 above).

| entry | SPEC places that link it | built from |
|---|---|---|
| Records hold producer facts | 4.3, 6.1, I5 | Draft 1 "Records hold producer facts", first paragraph: consumer values stored first, then raw facts with one pure derivation. Revision 2, "A recapture dropped a restored fold", third bullet: the recapture functions that make I5 checkable |
| What a property-IC record keeps about cases and folds | 5.2 | Draft 1, same entry, second paragraph (the combinations); revision 2, same section, opening and first bullet (`canBeMegamorphic` recorded under the native name, why the bit is unambiguous on a baseline IC); drain item 5, first half (`slowOperationKind` and `foldBits` became `stateBits` with `holdsGaveUp`) |
| A site comes back given up only when it lists no case | 6.1 | Draft 1 "`m_cacheType` ...", second paragraph; Draft 1 "The folded `instanceof` site"; revision 2 second bullet (given up and folded disjoint); revision 4 "Sites that gave up beside their cases"; drain item 1, first half (THREAD adopted the rule) |
| The shape group stays as installation builds it | 3.1, 6.4 | Draft 1 "`m_cacheType` is left as installation builds it", first paragraph |
| Every IC at a zero wait counter, every call site seen once | 6.1 | Draft 1 entry of that name |
| `resetByGC` travels | 3.1 | Draft 1 entry of that name |
| Dense records in native index spaces | 4.2 | Draft 1 entry of that name |
| A CB can have no metadata table | 4.2, 4.6 | revision 2 "A CB can have no metadata table", with the check names updated to S1 and S2 |
| The summary counts property ICs only | 5.3 | Draft 1 "What the summary counts" |
| The polymorphic bit, call sites included | 5.3, B4, B6 | revision 5 (no contract yet, the CB lane's floor uniform); revision 8 (residue across generations); drain item 2 (THREAD's bit, the call-site evidence, the sites that stay out, B6); walkthrough report 2 (the withheld mark in both CB sections) |
| Prepare reads the prepared `BaselineJITCode` | 6.2, R-IMG-1 | revision 4 "The ICs prepare had no code to read before `commit`" |
| Checks run only under strict and copy no native table | 4.5, 5.5, 6.2, E3 | Draft 1 "Strict and always-on checks" (both paragraphs); drain item 3; drain item 5, second half (C2 to C9, S1's fold clause and A5's operation clause removed); kept minor 1.7. State the debug assertions as the SPEC does (section 7, finding 3) |
| The IC limit options are fixed | 10, 4.3 (`caseCount`), 5.1 (no allocation) | Draft 1 "Options"; revision 2 smaller corrections (the `Vector` inline capacity); kept minors 1.1 to 1.9 |
| A capture's consistency comes from the pause | 5.1, L1 to L4 | Draft 1 "Locking"; kept minors 2.1 to 2.6; the native-fidelity item on L4 |
| One megamorphic predicate, one give-up table | E3, E4 | Draft 1 "One megamorphic predicate"; kept minors 3.5 to 3.7 |
| E1 stores `new.target` as the LLInt does | E1, T2, B5 | Draft 1 "E1 without a write barrier"; kept minors 3.1 to 3.4 (every baseline CB changes) and 3.8 to 3.10 with drain item 6 (the lane applies E1 itself) |
| Which capture the consumer imports in the tests | 11.2 (anchor), T3 to T5, T8, T10 | revision 3 "Which capture the consumer imports" (with the rejected second fix) and "T10's assertion on F proved less than it claimed"; kept minor 5.4 |
| The lane owns its test bindings' shapes | 11.1, R-INT-7, R-INT-9, T8, T12, T13, tasks 6 and 7 | revision 2 "The test bindings had no shape" and its smaller corrections (`jitcacheDelta` in test builds); revision 9 (task order); kept minors 4.8 to 4.10 (the snapshot built under `DeferGC`) |
| ICs runs keep concurrent JIT off | 11 intro, T9 | revision 6; drain item 4; the native-fidelity cross-set item; walkthrough report 3 |

### What the history drops

Logs and routine fixes, which git keeps: the header's provenance and Draft 1's THREAD revision line; every revision and round heading, the finding tallies and the "Reviews" list; Draft 1 "Executable-allocation fault hooks (provisional)", the E5 half of drain item 1 and revision 7, because THREAD Execution now assigns those sites and no lane decision remains; revision 3's smaller corrections; the drain's opening paragraph; kept minors 1.10, 2.7 to 2.10, 4.1 to 4.7 and 5.1 to 5.3, whose results the SPEC states with their reasons; the native-fidelity item on R-INT-9's justification; the Compaction table; the "Options table" and "Runner directives" notes; walkthrough report 1.

## 6. Map

### SPEC sections

| old | new | action | note or owner |
|---|---|---|---|
| 1 | 1 | keep | first paragraph merged into the list |
| 2 | 2 | keep | orientation only; two sentences go to 4.2 and 5.3 |
| 3, 3.1, 3.2, 3.3 | same | keep | derived values cite 6.1 |
| 4, 4.1 | same | keep | |
| 4.2 | 4.2 | keep | gains the dense-order reason |
| 4.3, 4.4 | same | keep | |
| 4.5 | 4.5 | keep | the lane's one statement of normal mode |
| 4.6 | 4.6 | keep | |
| 5 | 5 | keep | |
| 5.1 | 5.1 | keep | table cut (SPEC-integrator.md 9.1, 9.5 to 9.7, new 8.1, 8.5 to 8.7); consistency paragraph to L4 |
| 5.2 to 5.6 | same | keep | |
| 6 | 6 | keep | intro paragraph cut (SPEC-integrator.md 8.4, new 7.4) |
| 6.1 | 6.2 | merge | with 6.2 |
| 6.2 | 6.2 | merge | |
| 6.3 | 6.1 | move | the derivation leads |
| 6.4 | 6.3 | move | |
| 6.5 | 6.4 | move | |
| 6.6 | 6.5 | move | |
| 7 | 7 | keep | |
| 8, 8.1, 8.2, 8.3 | same | keep | |
| 9 | 9 | keep | |
| 10 | 10 | keep | retitled "Options" |
| 10.1 | cut | cut | owners: THREAD Failures and Restoration; SPEC-ics.md 4.5, 5.5, 6.2, I1, I2, E5, R-INT-2 to R-INT-4, R-INT-7 item 1; SPEC-integrator.md 8.2 step 9 and its closing paragraph (new 7.2), 8.3's `Abandoned` row (new 7.3), 9.4 step 4 (new 8.4) |
| 10.2 | 10 | merge | |
| 11, 11.1 | same | keep | |
| 11.2 | 11.2 | keep | the printing, `main(...)` and `Off` conventions become one bullet citing SPEC-integrator.md R-ALL-4, with the lane's clauses; T6 cites SPEC-integrator.harness.md section 7.6 |
| 12, 13, 14 | same | keep | |

### Rule IDs

Every rule ID keeps its name. Those that change section:

| IDs | old section | new section |
|---|---|---|
| S1, S2 | 6.1 | 6.2 |
| I1 to I5 | 6.6 | 6.5 |

All others keep their section: A1 to A8 (4.5), states (a) to (d) (5.2), C1 (5.5), E1 to E5 (7), R-INT-1 to R-INT-9, R-IMG-1, R-UCB-1, R-CB-1, R-CB-2 (8.2, 8.3; several shortened by citation, none removed), L1 to L5 (9; L4 gains 5.1's consistency paragraph), T1 to T14 (11.2; T6 becomes a citation, T9 gains a reason), B1 to B6 (12), M1 to M6 (13), tasks 1 to 9 (14). Sub-items keep their numbers (R-INT-7 items 1 to 4, T12 items 1 to 4, T13 items 1 and 2, T14 items 1 to 4).

### History

| old passage | new entry | action |
|---|---|---|
| header | header | merge, rewritten |
| Draft 1 opening line | none | cut: provenance |
| Draft 1 "Records hold producer facts", paragraph 1 | Records hold producer facts | merge |
| Draft 1 "Records hold producer facts", paragraph 2 | What a property-IC record keeps about cases and folds | merge |
| Draft 1 "Dense records in native index spaces" | Dense records in native index spaces | merge |
| Draft 1 "What the summary counts" | The summary counts property ICs only | merge |
| Draft 1 "Every IC at a zero wait counter, every call site seen once" | same title | merge |
| Draft 1 "`resetByGC` travels" | same title | merge |
| Draft 1 "`m_cacheType` ...", paragraph 1 | The shape group stays as installation builds it | merge |
| Draft 1 "`m_cacheType` ...", paragraph 2 | A site comes back given up only when it lists no case | merge |
| Draft 1 "The folded `instanceof` site" | A site comes back given up only when it lists no case | merge |
| Draft 1 "Executable-allocation fault hooks" | none | cut: THREAD Execution assigns the sites (SPEC-ics.md E5) |
| Draft 1 "E1 without a write barrier" | E1 stores `new.target` as the LLInt does | merge |
| Draft 1 "One megamorphic predicate" | One megamorphic predicate, one give-up table | merge |
| Draft 1 "Strict and always-on checks" | Checks run only under strict and copy no native table | merge |
| Draft 1 "Locking" | A capture's consistency comes from the pause | merge |
| Draft 1 "Options" | The IC limit options are fixed | merge |
| Revision 2 opening | none | cut: log |
| Revision 2 "A CB can have no metadata table" | same title | merge |
| Revision 2 "A recapture dropped a restored fold" | Records hold producer facts; What a property-IC record keeps ...; A site comes back given up ... | merge |
| Revision 2 "The test bindings had no shape" | The lane owns its test bindings' shapes | merge |
| Revision 2 "Smaller corrections" | The IC limit options are fixed; The lane owns its test bindings' shapes | merge |
| Revision 3 opening | none | cut: log |
| Revision 3 "Which capture the consumer imports" | Which capture the consumer imports in the tests | merge |
| Revision 3 "T10's assertion ..." | Which capture the consumer imports in the tests | merge |
| Revision 3 "Smaller corrections" | none | cut: routine; T5 and the keep-alive convention state it |
| Revision 4 opening | none | cut: log |
| Revision 4 "The ICs prepare had no code ..." | Prepare reads the prepared `BaselineJITCode` | merge |
| Revision 4 "Sites that gave up beside their cases" | A site comes back given up only when it lists no case | merge |
| Revision 5 | The polymorphic bit, call sites included | merge |
| Revision 6 | ICs runs keep concurrent JIT off | merge |
| Revision 7 | none | cut: no change to the lane |
| Revision 8 | The polymorphic bit, call sites included | merge |
| Revision 9 | The lane owns its test bindings' shapes | merge |
| Reviews | none | cut: log |
| Drain, opening | none | cut: log |
| Drain item 1 | A site comes back given up ...; E5 half cut (THREAD Execution) | merge |
| Drain item 2 | The polymorphic bit, call sites included | merge |
| Drain item 3 | Checks run only under strict and copy no native table | merge |
| Drain item 4 | ICs runs keep concurrent JIT off | merge |
| Drain item 5 | What a property-IC record keeps ...; Checks run only under strict ... | merge |
| Drain item 6 | E1 stores `new.target` as the LLInt does | merge |
| Kept minors 1.1 to 1.9 | The IC limit options are fixed (1.7 to the checks entry) | merge |
| Kept minor 1.10 | none | cut: routine; T9 states it |
| Kept minors 2.1 to 2.6 | A capture's consistency comes from the pause | merge |
| Kept minors 2.7 to 2.10 | none | cut: routine; T3 and E5 state them |
| Kept minors 3.1 to 3.10 | E1 stores ... (3.1 to 3.4, 3.8 to 3.10); One megamorphic predicate ... (3.5 to 3.7) | merge |
| Kept minors 4.1 to 4.7 | none | cut: routine; T12 item 2 states its reason |
| Kept minors 4.8 to 4.10 | The lane owns its test bindings' shapes | merge |
| Kept minors 5.1 to 5.3 | none | cut: routine; R-ALL-4 states the conventions they added, and 11.2 the lane's clauses |
| Kept minor 5.4 | Which capture the consumer imports in the tests | merge |
| Native-fidelity, L4 item | A capture's consistency comes from the pause | merge |
| Native-fidelity, R-INT-9 item | none | cut: a corrected justification; R-INT-9 states the requirement and its true reason in one clause |
| Native-fidelity, cross-set item | ICs runs keep concurrent JIT off; its T9 reason moves up into T9 | merge |
| Compaction (2026-10-07) | none | cut: log |
| Walkthrough report 1 | none | cut: routine; 11.2 states it |
| Walkthrough report 2 | The polymorphic bit, call sites included | merge |
| Walkthrough report 3 | ICs runs keep concurrent JIT off | merge |
| Options table | none | cut: log; section 10 points to options.md |
| Runner directives | none | cut: log; T9 states its directives |

## 7. Findings left unfixed

1. THREAD gap. THREAD Session classes a failed strict check as invalid material when it rejects bytes read from the artifact and as a recording fault when it rejects what a capture builds. S2 rejects neither: it checks the consumer's own newborn CB at install. SPEC-ics.md 6.1 and 10.1 call its failure invalid material, a class THREAD does not give for that case.
2. Stale restatement. 11.2 writes the script wrapper as `main(role, scratch, artifact)`, while SPEC-integrator.harness.md section 7.3, which owns it, passes four arguments and writes `main(role, scratch, artifact, sequence)`. A three-parameter wrapper still keeps the role out of the heap, so no decision differs; the cut to a citation removes the stale copy.
3. History against SPEC. Drain item 3 says debug builds assert A1 to A8, C1, S1, S2 and the two capture-call checks. The SPEC states debug assertions only for A1 to A6, the capture checks, and the A7 and A8 counts inside seeding and attach; S1, S2 and A7's per-mold pairing have none. The SPEC wins, and the rewritten history must state its rule. Whether S1 and S2 should also assert in debug builds is for the human.
4. Citation gaps, fixed in form by this plan: SPEC-image.md R-ALL-1 and R-ICS-1 place requirements on this lane that SPEC-ics.md never cites although E1, 4.2 and 6.4 meet them; task 5's "sections 6.1 to 6.5" included the derivation that task 3 implements; R-INT-6 cites SPEC-integrator.md section 11.1 for its step name, a section the integrator set cuts, so the rewrite cites section 5.4, whose site table names the step. SPEC-integrator.md section 9.4 step 6 cites section 5.6 for "the header word it wrote"; 5.6 says capture returns its summary and 4.1 lays the word out, so that citation still resolves.
