# Compaction plan: the ucb set

This plan covers SPEC-ucb.md, SPEC-ucb.codec.md and SPEC-ucb-history.md as they stand in this run's snapshot (`base/`). It changes form only: every requirement stays, in the place the map gives it, and every rule ID keeps its name. A plain section number such as §7.3.1 is SPEC-ucb.md's new number, "old §6.3.1" its old one, and "codec §4" a section of SPEC-ucb.codec.md, whose numbers do not change. A section of another file carries its snapshot number; where that set's plan renumbers it, this plan gives both, as in "SPEC-integrator.md old §10 (new §9)". The rewritten SPEC cites other sets by their snapshot numbers, as the rewrite asks, and the references pass retargets them. H1 to H26 label the history's new records (section 4 of this plan); the history uses their titles as headings, and the SPEC links them by those anchors. X1 labels the one move into another set (section 6), and G1 to G9 the gaps (section 7).

## 1. The order the reasoning takes

The main file defines the request points (old §6) before the body sections, checks and feedback layout (old §7 and §8) that each step of the engine reads. The engine, the densest part of the lane, therefore cites about a dozen layouts and checks the reader has not met. The file also opens with the owned-paths tables (old §3), whose rows cite sections the reader reaches only later. The new order defines what a body holds before the code that reads one: keys, then the lane's sections and their checks, then the feedback those sections carry, then the registry that remembers UCBs, then the request points, then capture. Interfaces, failures, options and invariants close the design. Tests, bench and the build plan (files, manifest, tasks) follow it.

Three regroupings help inside sections. Old §6.2.3, each request's key and context, joins the keys in §3, since it binds the key and context formats to request kinds. Old §4.3 splits into the request context (§3.3) and the UFE's holder digest with its TDZ chain and the digest-timing rule (§3.4): it ran two digest definitions and a timing rule through seven bullets and three paragraphs. Old §6.2.6 and §6.2.7 merge, since 6.2.7 only pointed back to 6.2.6 and 6.2.4.

New outline of SPEC-ucb.md, old sources in parentheses:

1. Scope and overview (preamble, old 1)
2. Native facts (old 2)
3. Keys, identities and contexts (old 4): 3.1 The body key (4.1); 3.2 Identity records (4.2); 3.3 Contexts (4.3, first part); 3.4 Holder digests and when digests are computed (4.3, the rest); 3.5 Source digests (4.4); 3.6 Each request's key and context (6.2.3); 3.7 Functions (4.5); 3.8 SHA-256 (4.6)
4. The lane's sections of a body file (old 7): 4.1 to 4.5 (7.1 to 7.5)
5. UCB feedback (old 8): 5.1 to 5.7 (8.1 to 8.7)
6. The parent-key registry, pending imports and statistics (old 5): 6.1 What it holds (5.1); 6.2 Who records what (5.4); 6.3 Destructor hooks and lock rules (5.5, then 5.3); 6.4 Pending imports (5.2); 6.5 Statistics (5.6, with old 6.7's counting rules)
7. Request points (old 6): 7.1 Request objects (6.1); 7.2 Hooks (6.2), with 7.2.1 Program, module and indirect eval (6.2.1), 7.2.2 UFE bodies (6.2.2), 7.2.3 Direct eval (6.2.4), 7.2.4 Root UFEs (6.2.5), 7.2.5 Paths without a request point (6.2.6 and 6.2.7), 7.2.6 Supplied digests (6.2.8); 7.3 The engine (6.3), with 7.3.1 to 7.3.6 (6.3.1 to 6.3.6); 7.4 Parse fields (6.4); 7.5 Publication (6.5); 7.6 Threads, locks and GC (6.6); 7.7 Engine interface (6.7)
8. Capture (old 9): 8.1 Calls, 8.2 `buildSections`
9. Interfaces (old 10): 9.1 What the lane provides, 9.2 What the lane requires of the integrator
10. Failures and strict checks (old 11): 10.1 Outcomes, 10.2 Strict
11. Options (old 12)
12. Invariants (old 16)
13. Test obligations (old 13): 13.1 Self-tests, 13.2 Twins, 13.3 JS tests
14. Bench obligations (old 14)
15. Files, manifest and tasks (new heading): 15.1 Owned paths (old 3), 15.2 Manifest for `INTEGRATE-ucb.md` (old 15), 15.3 Tasks (old 17)

The registry follows the body sections because `UCBRecord` keeps the identity section's butterfly map and the statistics name the checks of §4.4; it comes right before the request points, its main client. F1 to F26 stay in one list in §2, in numeric order: three other files cite them by ID and most design sections cite several, so scattering them by first use would turn each later citation into a search. Invariants precede the tests that check them.

SPEC-ucb.codec.md keeps its order: it already goes from what the core is, to the entry points, the payload, the mode's rules, the WTF transport E4 needs, determinism and its tests.

## 2. Passages in SPEC-ucb.md

Every citation inside the set is renumbered by the map of section 5. "Link Hn" means the passage gains a link to that history record.

### Preamble and §1

- The opening line's list ("keys, the parent-key registry, every request point, the UCB core with its parse fields and child descriptors, UCB feedback with the LLInt counter, and pending imports") is cut to "Lane 1 of THREAD Execution": it restates THREAD Execution's lane 1, which holds it.
- Keep the codec bullet and the definition of "THREAD". The history link says what SKILL.md says a history is: the record of why non-obvious decisions were taken, binding nothing.
- Old §1's first paragraph: keep.
- The overview keeps one sentence per flow: the key from the snapshot and the context only when read; import where the native path would generate; seeding where it decodes an embedder's bytecode cache; attach where a holder already has a UCB; a miss records what the native path produced; normal mode runs only the comparisons that choose a body while strict also validates; capture writes three sections and the lane's share of richness. The import flow's list of steps goes, since §7.3.1 holds it.

### §2 Native facts

- "Each was verified in this pin." is cut: a status line, which SKILL.md excludes. No requirement.
- F24 loses only what SPEC-integrator.md N9 restates of where DFG and FTL plans fail for lack of executable memory: the `FailedFinalizer` at the `didFailToAllocate()` branches of `SpeculativeJIT::compile` and `compileFunction`, the two paths that set `FTL::State::allocationFailed` (`FTL::compile`, and `FTL::LowerDFGToB3::compileCallFFIImpl` when Bun's FFI invoke thunk could not be allocated) and `FTL::fail` in `DFG::Plan::compileInThreadImpl`. N9 holds them and cites F24 for the effects.
- F24 keeps its baseline sentence whole: `JIT::finalizeOnMainThread` returns `CompilationFailed` only for a null `BaselineJITCode`, which `JIT::link` returns only after `LinkBuffer::didFailToAllocate()`, and `BaselineJITPlan::finalize` then calls `dontJITAnytimeSoon`, which defers the body's LLInt counter indefinitely, and sets `m_didFailJITCompilation`. Its two "only"s are the evidence that a baseline plan fails only for lack of executable memory. SPEC-integrator.md old §10 (new §9) raises the baseline fault unconditionally on that evidence, citing N3 and F24 in its edit's comment, while N3 states just that the function returns `CompilationFailed` for a null `BaselineJITCode`, so no other file states the evidence.
- F24 also keeps what the DFG and FTL failures write: `FailedFinalizer` recording no cause and its other installers (`FTL::canCompile`, `performOSREntrypointCreation`, `canCompileUnlinked`, the `b3AlwaysFails*` options); `DFG::Plan::finalize` returning `CompilationFailed` for it and then calling `m_callback->compilationDidComplete`; the callbacks' writes (`didFailDFGCompilation` setting the quick DFG bit to `False`, `didFailFTLCompilation` clearing the quick FTL bit, and the counter deferrals and `m_didFailFTLCompilation` beside them); and when the baseline CB is the replacement again, so that a later capture reads them.
- Every other fact keeps its claims and symbols, rewritten so each sentence states one native fact. The consequences sections cite stay: F2's "cannot be keyed by source range", F16's "empty code-generation mode", F17's fixed set of exit sites.

### §3 Keys, identities and contexts (old §4 and old §6.2.3)

- §3.1: keep.
- §3.2: keep. The `strict` and `withScope` bullet keeps its decoded-root clause in short and cites §7.3.3 step 1, which holds the rule.
- §3.3 Contexts: old §4.3's opening paragraph, its table, and the bullets on `providerOffset`, `firstLine` (link H1), a direct eval's lexically scoped features, the sorted TDZ and private-name sets, and canonical strings.
- §3.4 Holder digests and when digests are computed: old §4.3's holder-digest bullet; its TDZ chain bullet with move-up U-2 (link H2); its paragraph on the kept environment digest, cut to one sentence citing THREAD Session for the exemption; its paragraph on why a child's context holds only kind, offset and first line and where C11 compares; and its paragraph on when a context or holder digest is computed and what a record keeps (`RecordedContext`; link H3).
- §3.5 Source digests: keep the encoding and the three forms. The long trust paragraph becomes short sentences in this order: a supplied digest is trusted as `hash()` is; form 1 belongs to the build and is checked by U9 and debug assertions, never by strict; form 2 is checked by S2 at the first import that relies on it, once per provider, because an import replaces generation on the key alone while a decoded or attached UCB is matched by its core; in a VM that imports with strict on, the registry keeps the provider beside every key derived from such a root; a direct eval needs no check, since its key ends in the digest of its own text; twins builds with a twin report open check every supplied digest they take (T7). Link H4.
- §3.6 Each request's key and context: old §6.2.3 whole. Its sentence "The context column says what each digest covers; when it is computed and what a record keeps follow section 4.3" becomes a citation of §3.4.
- §3.7, §3.8: keep.

### §4 The lane's sections of a body file (old §7)

- §4.1: keep.
- §4.2: keep the layout. The closing paragraph keeps the parse rule per mode (citing §10.2), one sentence on each map's purpose (links H9 and H10) and the pointer to §7.3.2 for which UCBs a body serves.
- §4.3: keep, and add move-up U-1 (link H7).
- §4.4: keep the table. The closing paragraph keeps which checks run in which mode and when; its account of why an imported UCB needs no C10 and of what the butterfly map does is replaced by citations of §9.1, the atom contract, and §7.3.1 step 8.
- §4.5: keep.

### §5 UCB feedback (old §8)

- §5.1: keep the table. The LLInt counter row reads in THREAD's sense: base threshold and progress as captured, armed anew with no CodeBlock and so with no memory-pressure slice (§5.5; see G6). Keep the paragraph on plan failures and the one on children's and scope singletons. The root-bit paragraph becomes one sentence: a root UFE's own bit stays local (THREAD Restoration), and no part carries it or the `FunctionExecutable`'s `m_singleton`. THREAD Restoration holds that rule and both its costs; the two readers (`ByteCodeParser::get` with the `GetCallee` case of `AbstractInterpreter`, and `ObjectAllocationProfileBase::initializeProfile`) move to H14. Link H14.
- §5.2: keep. The first rule keeps its reason, that native profiles see only boxed values; its aside that the CB lane bounds its copies the same way is cut (SPEC-cb.md V6 holds it).
- §5.3: keep.
- §5.4: keep, with the R-UCB IDs as citations.
- §5.5: keep the code and the paragraph that compares it with native `setThreshold` (link H15). The deferred-counter paragraph keeps that a deferred record is armed exactly and that the format admits it; the evidence that no native capture holds one moves to H15.
- §5.6: keep the struct and its first paragraph. The second paragraph becomes "Every exit site counts (THREAD Capture)" with link H16. Its account of the polymorphic rule restates THREAD Restoration, SPEC-ics.md §5.3 and SPEC-cb.md §4.3, and its argument for why counting every site is safe moves to H16.
- §5.7: keep.

### §6 The parent-key registry, pending imports and statistics (old §5)

- §6.1: keep the declarations. The paragraph keeps the reason for `ParentIdentity`, the `SourceID` rule and `recordCodeBlock`'s locking, and adds move-up U-5. Link H17.
- §6.2 Who records what (old §5.4): keep the table. The two rows on UCB records cite §7.3.1 step 12, §7.3.3 steps 2, 3 and 7, and §7.3.5 for their fields instead of listing them. Keep the closing paragraph.
- §6.3 Destructor hooks and lock rules: old §5.5, then old §5.3. The hooks run on the sweeping thread, possibly inside a JITCache step (F12), which is why the lock is a leaf, so the merged text reads in that order. Link H17.
- §6.4 Pending imports (old §5.2): keep the class and the lifetime rule, citing THREAD Restoration. The bullets become what each call does: `pendingImport` returns the attached import; `resolvePendingImport` with `Installed` detaches it; with `DroppedByGate` it also stamps the import's index token as missed and counts `gateDrops`, because the gate would drop a re-attached import again. Two things are cut, since SPEC-integrator.md old §8.2 (new §7.2), steps 1, 5, 6 and 17, and old §8.3 (new §7.3)'s outcome table hold them: when the install glue makes each call, and the last bullet, that it makes none after a baked-fact mismatch or invalid material. Old §11.1's sentence that a pending import attached before activity went off stays and dies with its UCB moves here.
- §6.5 Statistics: old §5.6, plus old §6.7's rules for which digests increment `sourceDigests`, `suppliedSourceDigests`, `contextDigests`, `holderDigests` and `tdzEnvironmentDigests`, so each counter's rule sits with its declaration.

### §7 Request points (old §6)

- §7.1: keep. Its paragraph on the snapshot is the one statement of the snapshot rule (link H5); §3.2, §3.6, §7.2.1, §7.2.3 and I20 cite it.
- §7.2.1: keep.
- §7.2.2: keep, including the paragraph on the decoded slot nobody requested (link H19; see G1).
- §7.2.3: keep. The sentence on why the request object is built after the executable becomes a citation of §7.1 and F18.
- §7.2.4: keep. The `decodeBuiltinFunction` bullet cites codec E9, which holds that edit.
- §7.2.5 Paths without a request point: one list from old §6.2.6 and §6.2.7. Bun's `cachedData` decodes come first, under THREAD Restoration, with old §6.2.6's consequences: no record; a child's request misses `NoKey`; `captureRecord` finds no record; `vm.Script`'s runs, and `vm.compileFunction` without `cachedData` or with data its decode rejects, import at the program request point; the lane edits nothing in Bun. Then `JSC::evaluate` with a precompiled block, generation without an executable, and the debugger's direct eval, citing §7.2.3. Link H21.
- §7.2.6: keep.
- §7.3's preamble: keep.
- §7.3.1: keep. The stack paragraph keeps the rule and one sentence of reason (link H22). Step 4 splits into 4a, the stored key (invalid material in both modes), 4b provenance, 4c context, 4d holder (C11) and 4e S2, a sentence or two each, so each states one check and the later step numbers, which other passages cite, do not move. The closing line on an abandoned UCB stays here; old §11.1's copy goes.
- §7.3.2: keep. Its paragraph on stamps is the one place stamping is defined (link H18), and the provenance rule carries link H12.
- §7.3.3: keep. Step 1 holds the with-scope rule (link H21).
- §7.3.4: keep. The introduction's restatement of THREAD Identity becomes a citation (link H20).
- §7.3.5 Records: keep the first paragraph. The second, on what every record keeps of its context, restates §3.4 and is cut.
- §7.3.6 Roots: keep, and add move-up U-4.
- §7.4, §7.5: keep.
- §7.6: keep. The root hooks' notes cite §7.3.6 instead of restating it.
- §7.7: keep the declarations and the mapping of request members to engine calls; the counting rules move to §6.5.

### §8 Capture (old §9)

- Keep. The Provenance bullet of §8.2 step 5 keeps its reason in two sentences (link H12).

### §9 Interfaces (old §10)

- §9.1: keep the provisions to the integrator; the options bullet cites §11. The CB and Image accessors cite §5.4. The index-space paragraph keeps the guarantee, how it holds (I7) and what strict adds; its enumeration of the index spaces is cut, since SPEC-image.md R-UCB-1 lists them (with SPEC-cb.md R-UCB-2 and SPEC-ics.md R-UCB-1). The string-constants paragraph is the one statement of the atom contract (link H9). Keep the constant-cells paragraph and the provisions to every part.
- §9.2: R-INT-1 to R-INT-10 keep, each trimmed to what the lane codes against.
  - R-INT-3 keeps the contract the engine relies on: `bodyVersion` answers a token that is nonzero while the index lists a body for the key and changes whenever it learns of another body there, with no filesystem call, and may lag other processes; `openBody` answers `Missing`, `Unusable` (the fault already raised) or `Found`, and `Found` only after the integrity checks in both modes and the structure checks with strict on; `ValidatedBody::version()` is the commit identifier, compared only with commit identifiers; `ValidatedBody`'s lifetime, thread and alignment guarantees; neither call allocates a JSC cell. Its account of how the integrator meets this is cut: SPEC-integrator.md old §7.2 (new §6.2) holds the lookup, container sub-SPEC section 4.5 what the integrity and structure checks are, and its sections 6 and 7 the index and the reads. Its bench sentence merges into B4.
  - R-INT-4 drops its aside on SPEC-ics.md R-INT-4.
  - R-INT-10 keeps that the runner exists in every build, since §13 runs the lane's tests with twins and without them; that clause rests on the build matrix §13 keeps (G7).
  - R-INT-11 keeps the requirement: the integrator raises the executable-allocation fault through `didFailExecutableAllocation` at each tier's plan site before any of the failed plan's effects (F24) are written, the CB lane's counter deferral included, and a plan that fails for another reason records nothing and keeps its effects, which then travel. It cites SPEC-integrator.md's plan-site section, old §10 (new §9), for where the calls go (link H26). The rest is cut, each part to the place the integrator plan gives it:
    - the placement of both calls, the `DFG::Plan` flag with the four branches where the compiling thread sets it, the worklist lock that orders the flag's write before its read, and that both calls run inside the plan's finalization with no worklist lock held: SPEC-integrator.md old §10 (new §9), with N9 for the branches;
    - what the DFG and FTL callbacks write: F24, which this set keeps, and old §10 (new §9), which keeps why the fault precedes the callback;
    - the baseline bullet's reason, that the case needs no cause because a baseline plan fails only for lack of executable memory: old §10 (new §9)'s baseline edit, whose comment cites N3 and F24; F24 keeps the evidence (see "§2 Native facts" above);
    - the entry point's signature, its plan-site enumerators and its contract (it runs on the VM thread, allocates no cell, does not stop for the collector and waits for no thread): SPEC-integrator.md old §5.4 (new §4.5), its declarations and the paragraph that makes the call safe inside a plan's finalization;
    - the GC deferrals the plan sites run under, the `DeferGC` of `completeAllReadyPlansForVM` or the deferral a synchronous route's caller holds: SPEC-integrator.md old §12 (new §10), whose GC paragraph the integrator plan gives them from old §10, with N8;
    - that Bun's `vm.Script` route reaches `BaselineJITPlan::finalize` through `JIT::compileSync`: N2, and old §12 (new §10)'s one statement of that route, which the integrator plan merges there from old §8.4, §9.5, §9.7 and §10. That statement also says the route's finalization runs with no deferral, which old R-INT-11's third bullet contradicts (G8).
  - The closing paragraph is cut, sentence by sentence:
    - which part calls `didFailExecutableAllocation` at which sites: THREAD Execution, and SPEC-integrator.md old §5.4 (new §4.5)'s site table;
    - that the list is exhaustive and the other `JITCompilationCanFail` sites, in Yarr, Bun's FFI thunks and stubs and WebAssembly, keep native behavior: THREAD Failures. Its clause that those sites write nothing a capture reads is the evidence for that list. Neither THREAD nor the integrator set states it, so it moves to H26;
    - that an FFI invoke thunk an FTL plan cannot allocate still reaches the plan site, through `FTL::State::allocationFailed`: SPEC-integrator.md old §10 (new §9)'s FFI paragraph, which the integrator plan makes the one place for that case (its new §4.5 drops its copy and cites it), and N9, which holds the mechanism. THREAD Failures does not state it, and F24 loses that path to N9 (above), so this set reaches the case only through R-INT-11's citation of new §9.

### §10 Failures and strict checks (old §11)

- §10.1: keep the table, without parenthetical step numbers that §7.3 already gives. Of the closing paragraph keep the first sentence; the other two duplicate §7.3.1's closing line and §6.4.
- §10.2: the introduction keeps the split between modes in three sentences (link H23). Keep the table, the two sentences on why an import is not re-encoded (link H7) and the sentence on S1. The paragraph on S2 is cut: §3.5 holds it.

### §11 Options (old §12)

- Keep the first sentence and add M5's "`OptionsList.h` is unchanged". The second sentence, that the integrator's table captures each required value at build time, is cut: SPEC-integrator.md old §6.3 (new §5.1) and options.md's fixed table hold how the values are set and checked (see G5). §9.1's options bullet, R-INT-9 and M5 cite §11.

### §12 Invariants (old §16)

- Keep I1 to I26. I13 keeps the property and cites E1, E2, E4, E6, E10, E11 and E14 for what the codec writes. I16 keeps the overshoot bound and cites E8 and §8.2 for how charges are made.

### §13 Test obligations (old §13)

- The introduction keeps the build matrix as it stands, because no other file holds its release half and HARNESS.md and SPEC-integrator.md old §16 (new §15) prescribe other builds (G7). It states that every JS test runs through the runner (R-INT-10), in the `ucb/` directory's default sequence unless it declares others, in a debug build with twins on and, unless it declares `// jitcache-requires: twins`, in a release build without them, where the oracle comparison (harness §7.6) is its check; every test runs with strict on (THREAD Verification), and a test that also checks the default adds a strict-off run and says so; a test whose sequence cannot run without a twins-only helper, such as one that rewrites a section with `jitcacheRewriteSection` or calls `jitcacheDelta`, declares `// jitcache-requires: twins`. "Where the oracle comparison is its check" and the twins declaration with its two examples come from old §13.3's fifth convention bullet, so the release half is stated once.
- The introduction cuts its opening pointer ("The oracle, the native twins and the capture records are THREAD Verification's"), the producer-then-consumer sequence in fresh processes, which the directory's default sequence replaces (harness §7.3 and §7.4), and its account of the oracle, which compares every run that configures JITCache, producers included, with a JITCache-off run on output and, for a jsc-hosted script in twins mode, on the reachable heap (harness §7.6 and THREAD Verification). Once that account is gone, §13 no longer says what the oracle compares, so each passage of the set that names the comparison cites harness §7.6, which does: the introduction's "the oracle comparison" (above) and §13.3's convention sentence (below).
- §13.1: keep (link H24).
- §13.2: keep; the last sentence of the `verifyRegistry` paragraph cites M4 instead of restating it.
- §13.3: keep the opening paragraph, on the directory, the Bun marking and what "Statistics" means. Keep the sentence that introduces the conventions, with its citations of R-ALL-4 and harness §7.2 to §7.7; in its closing clause, "since the oracle compares each of their JITCache runs with an `Off` run (section 13)", the citation becomes "(harness sub-SPEC section 7.6)", the section that says the runner compares each run that configures JITCache with an `Off` run. Convention bullets 1, 2 and 4 are cut: SPEC-integrator.md R-ALL-4 and harness §7.2, §7.3 and §7.7 hold them, and bullet 1 is stale (G4). Bullet 5 moves its release clause and its twins declaration into the introduction (above); its list of the helpers that exist only in twins builds and its rule to assert on them only where they exist are cut, since R-ALL-4, harness §5.2 to §5.4 and M4 hold them. Bullet 3, the `Off` role, moves to R-ALL-4 (move X1, section 6) and keeps here only "without statistics" with a citation of R-ALL-4, or stays whole if R-ALL-4 does not hold it.
- §13.3's table: keep, `compile-function.js`'s `cachedData` case that calls the user function only in release runs included (G7). Two clauses go. `atom-constants.js` drops turning `useConcurrentJIT` off in every run, which harness §7.4 adds to every twins-mode run of the directory and §7.7 passes to Bun as `BUN_JSC_useConcurrentJIT=0`. The stress line drops "with `--destroy-vm`", which harness §7.3 passes to every jsc run and §7.7 replaces with `BUN_DESTRUCT_VM_ON_EXIT=1`; it keeps `--collectContinuously=true` and `--useUnlinkedCodeBlockJettisoning=true`, "every debug run ... ASan and LSan clean" and the `verifyRegistry` check.

### §14 Bench obligations (old §14)

- Keep B1 to B10. B4 absorbs R-INT-3's bench sentence. The accounting paragraph drops its first sentence, which restates THREAD's opening, and keeps the partition of the lane's parts (link H25).
- B2's next-to-last sentence keeps when and how the request point records its parts: when `VM::jitCacheState()->benchReport()` is non-null, it records them with `BenchReport::record` in the `request` event (harness §9.2). Its list of the event's fields (the key, what the request point did, each part's time and the two totals) is cut, since harness §9.2's `request` row lists the same fields, and so is its closing clause, that IB3 joins the event with the `install` event by key, which SPEC-integrator.md IB3 states. That row calls each part's time CPU time; B2's last sentence settles the clock.
- B2's last sentence stays whole, with its citation of harness §9.1: a total reads the thread CPU clock only where its counted span begins and ends, and the per-part breakdown reads `CLOCK_MONOTONIC` (`MonotonicTime::now()`), so no system call lands inside a span the bound counts. Harness §9.2's `request` row records "the CPU time of each part B2 lists", while §9.1 has a breakdown of a counted span into steps read `CLOCK_MONOTONIC`, with the install function's single span as its example, and the integrator plan keeps both passages as written (its G10). This sentence is therefore the only one that says which clock times each part of the `request` event (G9).

### §15 Files, manifest and tasks (old §3, §15, §17)

- These three are the build plan, read once the design is known; at old §3 the owned-paths rows cited sections the reader had not met.
- §15.1: keep both tables. The unified-sources sentence becomes a citation of SPEC-integrator.md R-ALL-8, which names the `ucb` prefix.
- §15.2: keep M1 to M4. M5 becomes "the option rows of §11, in the integrator's table (R-INT-9)". M6 cites §7.2.6 for the declaration and keeps that the edit adds only that member, with standard types and no `jitcache/` include.
- §15.3: keep. Task 0's "sections 4 to 10 and 13.2" becomes "sections 3 to 9 and 13.2".

## 3. Passages in SPEC-ucb.codec.md

- Preamble: keep that the file is part of SPEC-ucb.md, that F1 to F26 and the invariants apply, and what the file specifies. Its sentence dividing the work among tasks 0, 3 and 4 restates the main file's §15.3 and is cut.
- Codec §1: keep (link H6).
- Codec §2: keep the declarations and the paragraph on the exported header. The paragraph beginning "The main file's capture adapts `ProducerBudget`" restates who passes a budget and who hashes chunks, which the main file's §8.2 step 3 and its Digests bullet, §3.7 (`holderDigest`) and §4.5 (`coreDigestOf`) hold; it is cut. Keep the paragraph on the descriptor (see G3).
- Codec §3: keep.
- Codec §4: keep E1 to E15. E4 gains move-up U-6 (link H8). E8 carries link H13. E9 keeps the edit and cites the main file's §7.2.4 for the hook's semantics. E10 cites F19 for the native decoder's behavior instead of retelling it (link H9). E11 carries link H11. E14 gains move-up U-3 (link H2).
- Codec §5: keep.
- Codec §6: keep every property. The bullets that only restate a rule shrink to a clause with its citation: the `RegExp` bullet (E11), the bullet on checksums, updatable fields and child bodies (E1 and section 4's opening), and the TDZ bullet (E14). The paragraph on `VariableEnvironment` keys stays, since options.md cites it.
- Codec §7: keep.
- Every "main file, section N" citation is renumbered.

## 4. The history

### 4.1 What it becomes

SPEC-ucb-history.md is a log today: nine design revisions in date order, each ending with a list of THREAD gaps, then review rounds, a drain, sixty-odd kept minors, a compaction map, report drains, walkthrough reports and two moves of content to other files. SKILL.md asks for the path to each non-obvious decision instead: what first seemed right, the evidence that changed it and the intent that came out. The new history is 26 records in the SPEC's order, one per decision, each merging every revision and review that touched it, so a superseded first idea appears once, as the starting point of its record, and never as a record of its own. H26 was added last, so its label is out of sequence: in the file it follows H22, where R-INT-11 falls in the SPEC's order. Each record cites the SPEC by rule ID and new section number. A one-line opening says what the file is and that nothing in it binds.

### 4.2 Records

Each entry gives the record's sources and what it keeps.

- H1. What a context holds. Sources: first draft, "Contexts hold every generation input outside the key"; revision 3, "Contexts record the source's first line". Keeps: THREAD's context list read as examples, and a child given only its offset; the evidence that any generation input outside key and context lets two different UCBs share a body, and that three child-descriptor fields hold absolute lines (F21), so two `vm.Script`s of one text under different `lineOffset`s produced different UCBs under one key and context (wrong stack lines, a line near 2^31, a strict fault at an attach); the intent that every context records its first line and that a field initializer differs only through its holder (C11). The start column was refuted, since it shapes no UCB; rebasing absolute lines at decode was rejected (the decoder would need the request's source, the encoder would rewrite three fields against a clamped base, and a decoded UCB would stop encoding to its payload's bytes, all for a rare case).
- H2. Holder digests and TDZ chains. Sources: revision 2, "The holder UFE pins a function body's generation inputs"; revision 9; revision 8's paragraph on build-time holder digests. Keeps: a decoded parent's key pins its children only if the payload's generator ran with the fixed options, hence C11 wherever a match relies on generation inputs; the descriptor as the codec's encoding of the UFE alone (E1, E2), replacing a hand-listed creation digest; the finding that every function of a module holds a chain naming each module binding (F26), so holder digests, cores, decodes and core digests grew with the enclosing scope; the intent: E14's start record, equality by interned environment, the chain digest, the environment digest kept in `CompactTDZEnvironment` with no link memo, and that digest uncharged (THREAD Session now says so). The finding's own fixes (a marker after the generation's own links; memoizing links) and build-time holder digests for builtins (a second definition of the descriptor in Python) were not adopted.
- H3. Contexts are digested where they are read. Source: revision 8. Keeps: the first design computed the context with the key at every request and stored the digest in every record; a root body's context holds a descriptor encode and a direct eval's sorts every TDZ and private name of the scope chain (`JSScope::collectClosureVariablesUnderTDZ`), mostly for keys with no body, against "no work ahead of demand"; the intent that requests compute contexts at the first comparison with a body, records keep inputs, and a direct eval keeps a digest computed at its record only while production is active, since a copy of its sets would keep every binding name per eval UCB. Skipping C11 for a root whose context matched proved unnecessary once the request keeps the holder digest.
- H4. Supplied root digests and their verification. Sources: revision 6, "Supplied root digests"; the drain's paragraphs on S2 and on the builtins section; the first draft's point on internal modules. Keeps: every root key digest once hashed the whole source, builtins at creation; F25's evidence that compiled apps, standalone internal modules and builtins never read that text natively, against THREAD Session's near-zero overhead; the intent of the three forms of §3.5, S2 at the first import relying on a provider's digest, none for a direct eval, and the builtins section's digest table located by the header's first reserved word because `src/exe_format/builtins.rs` reads another platform's executable with a fixed record size. Per-body text digests and a per-provider cache of computed digests were rejected.
- H5. Keys read a snapshot of the request's features. Source: revision 2, "Request features are snapshotted". Keeps: keys first read `lexicallyScopedFeatures()` lazily; F18: the request's own native steps overwrite that field, so producer and consumer computed different keys for a `"use strict"` program and a CodeCache hit replaced the with-scope bit; the intent that request constructors snapshot before any native step and keys and twins read only the snapshot.
- H6. The bytecode cache's codec in a JITCache mode. Sources: first draft, "The bytecode cache's codec, in a JITCache mode, rather than a new one", and its smaller point on symbol lookups. Keeps: a codec of JITCache's own would duplicate thousands of lines and drift from the engine, while the bytecode cache already encodes every part of a core, Bun maintains it and made it deterministic; the gaps it leaves (F7 to F10, F26) and the surgery at each; a failed symbol lookup becoming a decoder failure (E5), because a body naming a private name the VM lacks must be invalid material, not a crash.
- H7. A core's content is its encoding. Sources: first draft, "The core digest instead of a separate index-space walk"; revision 5, "Imports are not re-encoded, and digests hash the encoder's pages". Keeps: a canonical walk over the index spaces, then a strict re-encode of every import and a full native encode per match inside the request point (64 KiB zeroed first page, `Encoder::release` copying every page); the evidence that the deterministic codec is the canonical form and that re-encoding an import repeats a property of the build which T1 and U7 check; the intent: `coreDigest` hashes the section, imports are not re-encoded, the reuse check (now S1) stays, and E12 and E13 cut the cost. A hash-only walk of the fields was rejected: a second definition of the content, and a missed field would let two UCBs match silently.
- H8. Hashed declaration maps keep their layout. Source: first draft, "Global var order and `InlineMap` layouts". Keeps: the native decode re-adds entries, which reorders a map above nine entries (F10), and global var creation order is observable through `Object.keys(globalThis)` and the first redeclaration error; the intent of E4 and `InlineMap::restoreHashedLayout`, since no insertion order reproduces a layout in general.
- H9. Atom-ness travels beside the core. Sources: first draft, "String constants and atom-ness"; revision 1, "Atom-ness of natively decoded constants depends on decode history"; revision 2, the atom-ness half of "Provenance leaves the context; atom-ness leaves the core bytes"; revision 4, "The atom-ness guarantee is reconciled"; kept minors 4.4 to 4.6. Keeps: atom-ness first written into the core; the evidence that the strict-equality templates compare constant atoms by pointer, that generation makes every constant an atom while the native decoder makes long strings plain unless it already holds the atom, and that property-key uses atomize in place later (F19), so two decodes of one payload differed and a decoded function never matched a generated body; the intent: E10, the atom map, atomization of an unpublished decoded UCB's marked constants with `swapToAtomString`, which marks the cell as `toAtomString` does and reaches VM-wide cells other UCBs share as a property-key use would, C10 as a miss, and the one-way guarantee reconciled with SPEC-image.md R-UCB-1.
- H10. Butterflies generation builds from atom strings. Source: revision 3, "Imports rebuild the butterflies generation builds from atom strings". Keeps: the decode's plain butterflies; F23's `indexOf` and `includes` fast paths, which made an import differ from its twin; the intent of the butterfly map, acted on only by an import, interning each element through `atomStringToJSStringMap` because a switched structure alone makes `indexOf` return -1 wrongly; writing the form into the core would have broken matching a decoded UCB to a generated body.
- H11. `RegExp` constants by pattern and flags. Source: revision 3. Keeps: the native record's atom, specific pattern and parsed bit; F22's shared cell, cleared by `RegExp::deleteCode` whenever `VM::deleteAllCode` runs (`Bun.shrink()`), which made a core's encoding depend on code deletion and failed S1, matching and T1 on legitimate bodies; the intent of E11 and C5's validity check.
- H12. Provenance: embedder payloads match by content. Sources: revision 1, "Provenance is part of the context"; revision 2, "Embedder payloads are matched by content" and the provenance half of "Provenance leaves the context"; revision 3, "One provenance rule for every core kind"; kept minors 5.6 to 5.8. Keeps: equal key and context first taken as proof, then provenance in every context, then a provenance field checked for programs and modules only; the evidence of F4, F11 and the unrecorded options of a payload's generator, and of a function captured from a decoded slot replacing generation under other options; the intent that a decoded UCB matches only by core digest, in both modes, and that an `EmbedderDecoded` body serves only decoded UCBs of every kind. The miss revision 3 accepted for a ConsumerProducer's recapture of a decoded UCB was later fixed by keeping the matched `Generated` body's butterfly map and writing provenance `Generated` (§8.2).
- H13. Charges inside the encoder. Source: kept minors 1.1 to 1.9. Keeps: only pages charged and a one-page overshoot claimed; the evidence that tables, deferred closures and per-record scratch allocate too, and that a refused encode cannot unwind and pages double to 64 MiB; the intent of E8, I16 and B6, and of `UCBSections` holding its budget. A charged estimate before the encode (no count bounds an encode before it runs) and a reused scratch page (it would break the offsets later records and `release` read) were rejected.
- H14. Parse fields and singleton state. Sources: first draft, "Children's parse fields and learned bits"; revision 1, "Scope singletons and root singleton bits"; revision 5, "A root's own singleton bit stays native"; the native-fidelity review's first point; old §8.1's paragraph on the root bit (moved down). Keeps: children's parse fields as the producer left them, and the root bit read from the root's captured body at creation; the evidence that a parent encoded after some children ran carries parsed values (F3 makes the restore load-bearing), that `SymbolTable` singletons propagate from realm clones (F14), and that reading the root bit opened and validated every captured builtin body during global object setup (`JSC_BUILTIN_FUNCTION_WITHOUT_TRANSITION`, thirteen in `ArrayPrototype::finishCreation`); the intent: E2, the child bit with its parent, a bit per `SymbolTable` constant, and the root bit local, whose two readers and their speed-only costs this record names. Lazy validation of the root's body was rejected as still work ahead of demand.
- H15. Arming the LLInt counter. Sources: first draft, "The LLInt counter"; revision 2, "The LLInt counter keeps its progress exactly"; kept minors 6.1 and 6.2; old §8.5's paragraph on deferred counters (moved down). Keeps: a copy of `setThreshold` with no CodeBlock; F20's evidence that it adds the untruncated slice while `jitSoon` leaves fractional progress, so T4 failed on most bodies; the intent to truncate first, keeping `count()` at P with native crossing; the deferred record accepted and armed exactly, since `deferIndefinitely`'s state is native, although no native capture holds one (the LLInt defers only when `shouldJIT` fails for every CB, and `BaselineJITPlan::finalize` only after R-INT-11's fault). Rejecting `INT32_MAX` as invalid material was not taken.
- H16. Every exit site counts toward richness. Sources: revision 4, "Exit sites a consumer induced at polymorphic sites"; revision 7; the drain's point on exit sites; old §8.6's second paragraph (moved down). Keeps: THREAD's count with a gap noted; the evidence that a consumer's early DFG compile at a site whose IC held one shape took `BadCache` exits that jettison tallied into the UCB's exit profile, which only appends and which seeding carries, so a richer recapture wrote consumer-induced sites that slowed every later compile (`hasBadCacheExitSite`); the intent: THREAD's polymorphic rule removes the early compile, so every site counts; a lane-local exclusion was rejected because it would bind three parts; SPEC-integrator.md IB10 measures generations. SPEC-image-history.md points to this record, which keeps that pointer true.
- H17. The registry's lock, lazy child identities and shared parent nodes. Sources: first draft, "Lazy child identities" and "A lock on a VM-thread registry"; kept minors 2.1 to 2.7. Keeps: a VM-thread registry first needing no lock, eager child digests, and child entries copying the parent key; the evidence of F12 (a sweep can run a destructor inside a JITCache step), that most nested functions never run, and that a copied key made a child entry cost about as much as the UFE cell; the intent of the leaf lock, digests at the body's request and one `ParentIdentity` per recorded parent. A per-parent child vector (`identityOf` must work from the UFE alone) and reserving the maps were rejected.
- H18. Misses are stamped with index tokens. Sources: first draft, the miss flag of "Meeting a live UCB"; revision 1, "Misses expire with the body version"; the drain's paragraph on version stamps; kept minors 2.8 to 2.10 and 4.8. Keeps: a never-expiring miss flag, then a stamp of the opened file's commit identifier; the evidence that the flag kept a ConsumerProducer from reusing a UCB whose body it committed later, and that a commit identifier never equals the index's token; the intent: tokens from the in-memory index with no filesystem call, stamps compared with tokens, the matched-file shortcut with commit identifiers, one failed open per UCB for a body `compact` removed, and an `AtomMap` miss left unstamped with `matchedBodyVersion` set, since atom-ness only grows.
- H19. Decode points keep the native decode. Sources: first draft, "Import before the provider's cached bytecode"; revision 1, "Decode points keep the native decode and seed its result"; revision 2, "Settlement covers the requested slot only"; the drain's first point; kept minor 7.2. Keeps: the import replacing the provider's decode, and lazily decoded slots served as live; F1's evidence that the slots stay unpublished until `m_isCached` clears, and that replacing the decode would generate from source every descendant the producer never captured, a startup regression for `--compile --bytecode`; the intent, now THREAD Restoration's; the requested slot alone settling a request; and the other decoded slot's UCB feedback never arriving, since seeding it would read a body nobody requested (see G1).
- H20. A live UCB is met only at its holder. Sources: first draft, "Meeting a live UCB"; revision 2, "Attaches cost nothing until an attach can happen"; kept minor 4.7. Keeps: a global lookup of live UCBs by key, rejected because it would give one UCB to two holders; the first attach's whole-source digest at every CodeCache hit and re-encode at every re-attach; the intent of the ordered checks of §7.3.4 and the matched-version shortcut; atomizing at an attach rejected, since THREAD seeds only before publication.
- H21. Decoded roots and Bun's `cachedData` paths. Sources: revision 2's keying of decoded roots by their payload's bit; the drain's paragraphs on THREAD's settlement and on the freed `cachedData` span; the first draft's third gap and revision 2's fifth. Keeps: decoded roots keyed by their payload's with-scope bit and a lane edit to `NodeVM.cpp`; the evidence of F4 and F11; THREAD's settlement (no key for a decoded root whose bit differs; Bun's `cachedData` decodes stay native, with no Bun edit); and the use-after-free that happens with JITCache off too, which is why `compile-function.js` calls the user function only in release runs.
- H22. Parse-time stack depth. Sources: the first draft's first gap; kept minors 6.3 and 6.4. Keeps: importing regardless of depth, as a CodeCache hit does; the native `RangeError` an import would then skip; the intent of the `Stack` miss at `isSafeToRecurse`, the parser's own test, with THREAD Verification exempting deeper parses. A depth margin as a bench parameter was rejected (it misses imports where native parsing succeeds and still cannot predict a parse's depth).
- H23. Normal mode and strict. Sources: the drain's paragraphs on strict and on the decoder's shallow checks; kept minor 7.3. Keeps: every rule and C1 to C9 checked at every import; the evidence: THREAD made strict opt-in with normal mode checking integrity only, and the decoder's checks missed nested records; the intent: what chooses a body runs in both modes, C6 always, E15 as strict's validating decode, operands and targets left to the checksum and build ID (no JSC validator exists, and a rewritten artifact could carry wrong machine code as easily), and a stored key that differs from the body's own as invalid material in both modes, since files are named by their whole key.
- H24. Self-tests run once per configuration. Source: kept minor 6.9. Keeps: one call meant to cover U8; the evidence that `start` fixes role and strictness and invalid material turns activity off for good; the intent of `UCBSelfTestScope` and `self-test.js`; self-tests creating their own VMs was rejected.
- H25. Request-point work and the installation bound. Sources: revision 5, "Request-point work and THREAD's bound"; revision 9's extension; the drain's last point. Keeps: THREAD first counted only the seeding, leaving matching, closure checks, parses, atomization, parse fields and records in neither the bound nor the separate measurements; the evidence that all of it runs before the native link that opened THREAD's window, and that each TDZ environment's first digest is a key-side cost; the intent, now matching THREAD's opening: the partition of §14, with the butterfly rebuild inside the bound as JITCache work and strict's checks outside, since the bound is measured with strict off.
- H26. Plan-site faults go through the integrator. Sources: revision 6, "Plan-site faults"; the drain's bullet on plan-site faults. Keeps: no part first owned the executable-allocation fault of a DFG or FTL plan, though THREAD Failures required it before the failure's effects; the evidence of F24, that three of the writes a failed plan makes are this lane's state (the LLInt counter's deferral in `BaselineJITPlan::finalize`, the quick DFG and FTL bits through the DFG and FTL callbacks), that `FailedFinalizer` records no cause while a plan that fails for another reason must keep its effects, and that the other `JITCompilationCanFail` sites, in Yarr, Bun's FFI thunks and stubs and WebAssembly, write nothing a capture reads; the intent: R-INT-11, with a cause flag on `DFG::Plan`. THREAD Execution now names the plan sites, and THREAD Failures makes the list of the other sites exhaustive.

### 4.3 What the history drops

- The review records (rounds 1 to 3, system reviews 1 to 3, five-part reviews 1 to 3): logs. Each accepted finding's decision lives in one of the records above, with its evidence.
- Every "THREAD gaps raised" list: status lines. THREAD now settles all nine (Verification for stack depth; Restoration for UFE singleton bits, decode points, `cachedData` paths and the polymorphic rule; Identity for decoded roots; the opening for request-point work; Execution and Failures for plan-site faults; Session for the kept digests).
- First draft: "SHA-256" (§3.8 states the implementation; nothing non-obvious), the smaller points on extra memory (§7.3.1 step 11 states the amount) and on a direct eval's holder (§7.5).
- Revision 1: "Interfaces frozen by a header task" (log of a task-plan fix) and "Direct eval site is optional" (§7.2.3 states the rule with its reason).
- Revision 5: "The twin report keeps skips apart": a log; SPEC-image.md old §17.2 (new §11) and harness §7.4 hold the skip rule.
- Revision 8: "Task plan" (log).
- The drain's list of marks that became plain statements (status), once its substance is in H4, H16, H19, H21, H22, H23, H25 and H26.
- The kept minors not named in section 4.2 (1.10's signature changes, 3.1 to 3.10, 4.1 to 4.3, 4.9, 4.10, 5.0 to 5.5, 5.9, 5.10, 6.5 to 6.8, 6.10, 7.1): routine fixes, which git keeps.
- The native-fidelity review's points after the first: routine fixes and cross-set items whose rules now sit in B2 (the `request` event and its clocks), harness §9.1 and §9.2, SPEC-integrator.md IB3, R-ALL-2 and R-ALL-8, and SPEC-image.md I20. The clock rule's path, from a thread CPU clock read around each part to `CLOCK_MONOTONIC`, is the integrator history's record "Measurement choices", which the integrator plan links at harness §9.1.
- "Compaction (2026-10-07)", "Report drain", "Walkthrough reports", "Options table" and "Runner directives": logs.

### 4.4 What moves up into the SPEC

These are the one-sentence reasons without which an implementer would likely build the obvious version:

- U-1, §4.3 (from H7): matching encodes a UCB with the codec instead of walking its fields, so the codec stays the one definition of a core's content; a walk that missed a field would let two different UCBs match.
- U-2, §3.4 (from H2): the environment digest lives in the environment because a map keyed by its address would have to pin it, keeping a scope's names alive after the code that declared them died, or hook `CompactTDZEnvironmentMap::Handle::~Handle`, which cannot reach the VM; link digests are recomputed, and only environment digests are kept.
- U-3, codec E14 (from H2): chains compare by environment object rather than by link, because a natively decoded UCB's links are fresh objects while its environments are interned, so comparing environments cuts its chains where a generated UCB's are cut (I14).
- U-4, §7.3.6 (from revision 5): the identity digest runs at the root's creation because the UFE does not keep the root source it covers.
- U-5, §6.1 (from H17): a child's identity digest waits for its body's request because most nested functions never run (THREAD Session: no work ahead of demand).
- U-6, codec E4 (from H8): no insertion order reproduces a hashed layout in general, since growth rehashes and deletions leave tombstones, hence the checked restore of codec §5.

### 4.5 Links from the SPEC

Besides the header link, each record is linked beside the decision it explains: H1 at §3.3's `firstLine` bullet; H2 at §3.4's TDZ chain bullet and codec E14; H3 at §3.4's timing paragraph; H4 at §3.5's trust sentences; H5 at §7.1's snapshot paragraph; H6 at codec §1; H7 at §4.3 and §10.2's sentences on re-encoding; H8 at codec E4; H9 at §4.2's atom map, codec E10 and §9.1's atom contract; H10 at §4.2's butterfly map and §7.3.1 step 8; H11 at codec E11; H12 at §7.3.2's provenance rule and §8.2's Provenance bullet; H13 at codec E8; H14 at §5.1's singleton paragraphs; H15 at §5.5; H16 at §5.6; H17 at §6.1 and §6.3; H18 at §7.3.2's stamping paragraph; H19 at §7.2.2 and §7.3.3; H20 at §7.3.4; H21 at §7.2.5 and §7.3.3 step 1; H22 at §7.3.1's stack paragraph; H23 at §10.2; H24 at §13.1; H25 at §14's accounting; H26 at R-INT-11 (§9.2). Every passage must still read whole without its link.

## 5. Map

### 5.1 SPEC-ucb.md sections

| old | new | action | note |
|---|---|---|---|
| 1 | 1 | keep | with the trimmed preamble |
| 2 | 2 | keep | |
| 3 | 15.1 | move | |
| 4 | 3 | move | |
| 4.1 | 3.1 | move | |
| 4.2 | 3.2 | move | |
| 4.3 | 3.3 | move | holder, TDZ chain and environment digests, the child-context paragraph and the timing paragraph go to the new 3.4 |
| 4.4 | 3.5 | move | |
| 4.5 | 3.7 | move | |
| 4.6 | 3.8 | move | |
| 5 | 6 | move | |
| 5.1 | 6.1 | move | |
| 5.2 | 6.4 | move | |
| 5.3 | 6.3 | merge | after 5.5's text |
| 5.4 | 6.2 | move | |
| 5.5 | 6.3 | merge | with 5.3 |
| 5.6 | 6.5 | move | gains old 6.7's counting rules |
| 6 | 7 | move | |
| 6.1 | 7.1 | move | |
| 6.2 | 7.2 | move | |
| 6.2.1 | 7.2.1 | move | |
| 6.2.2 | 7.2.2 | move | |
| 6.2.3 | 3.6 | move | |
| 6.2.4 | 7.2.3 | move | |
| 6.2.5 | 7.2.4 | move | |
| 6.2.6 | 7.2.5 | merge | with 6.2.7 |
| 6.2.7 | 7.2.5 | merge | |
| 6.2.8 | 7.2.6 | move | |
| 6.3 | 7.3 | move | |
| 6.3.1 | 7.3.1 | move | step 4 becomes 4a to 4e; the other step numbers stay |
| 6.3.2 | 7.3.2 | move | |
| 6.3.3 | 7.3.3 | move | |
| 6.3.4 | 7.3.4 | move | |
| 6.3.5 | 7.3.5 | move | second paragraph cut (held by 3.4) |
| 6.3.6 | 7.3.6 | move | |
| 6.4 | 7.4 | move | |
| 6.5 | 7.5 | move | |
| 6.6 | 7.6 | move | |
| 6.7 | 7.7 | move | counting rules to 6.5 |
| 7 | 4 | move | |
| 7.1 | 4.1 | move | |
| 7.2 | 4.2 | move | |
| 7.3 | 4.3 | move | |
| 7.4 | 4.4 | move | |
| 7.5 | 4.5 | move | |
| 8 | 5 | move | |
| 8.1 to 8.7 | 5.1 to 5.7 | move | each n to 5.n |
| 9 | 8 | move | |
| 9.1 | 8.1 | move | |
| 9.2 | 8.2 | move | |
| 10 | 9 | move | |
| 10.1 | 9.1 | move | |
| 10.2 | 9.2 | move | |
| 11 | 10 | move | |
| 11.1 | 10.1 | move | |
| 11.2 | 10.2 | move | |
| 12 | 11 | move | |
| 13 | 13 | keep | the build matrix stays, with old 13.3's release clause (G7); the oracle restatement is cut, and "the oracle comparison" cites harness 7.6 |
| 13.1 | 13.1 | keep | |
| 13.2 | 13.2 | keep | |
| 13.3 | 13.3 | keep | convention bullets 1, 2 and 4 cut; bullet 5's release clause and twins declaration move to 13's introduction, the rest cut; bullet 3 per X1; the convention sentence cites harness 7.6 for the oracle instead of section 13 |
| 14 | 14 | keep | |
| 15 | 15.2 | move | |
| 16 | 12 | move | |
| 17 | 15.3 | move | |

New numbers with no old section: 3.4 (from old 4.3) and 15 (the heading over 15.1 to 15.3).

### 5.2 SPEC-ucb.codec.md sections

Codec §1 to §7 keep their numbers and titles.

### 5.3 Rule IDs

Every ID keeps its name. The table gives each family's new home.

| IDs | old home | new home | note |
|---|---|---|---|
| F1 to F26 | §2 | §2 | F24 loses only its DFG and FTL sites and keeps its baseline sentence whole (section 2 of this plan) |
| C1 to C11 | §7.4 | §4.4 | |
| SV1 to SV3, S1, S2, SC1, SC2 | §11.2 | §10.2 | |
| R-INT-1 to R-INT-11 | §10.2 | §9.2 | R-INT-3, R-INT-4 and R-INT-11 trimmed; R-INT-11 cites SPEC-integrator.md old §10 (new §9) and links H26 |
| I1 to I26 | §16 | §12 | I13 and I16 cite the codec rules |
| U1 to U9 | §13.1 | §13.1 | |
| T1 to T7 | §13.2 | §13.2 | T7 is defined in `verifySuppliedDigest`'s comment and the paragraph after the list |
| B1 to B10 | §14 | §14 | B2 drops the `request` event's field list and its IB3 clause and keeps its clock sentence whole (G9); B4 absorbs R-INT-3's bench sentence |
| M1 to M6 | §15 | §15.2 | M5 and M6 cite §11 and §7.2.6 |
| E1 to E15 | codec §4 | codec §4 | E4 and E14 gain a reason; E9 cites §7.2.4 |
| tasks 0 to 12 | §17 | §15.3 | numbers unchanged |

### 5.4 History

| old heading | new home |
|---|---|
| preamble | the one-line opening |
| Design record, first draft | H1, H6, H7, H8, H9, H14, H15, H17, H18, H19, H20, H22, and H4 for the internal-modules point; "SHA-256", two smaller points and the gap list cut |
| Revision 1 | H9, H12, H14, H18, H19; "Interfaces frozen by a header task", "Direct eval site is optional" and the gap list cut |
| Revision 2 | H2, H5, H9, H12, H15, H19, H20, H21; gap list cut |
| Revision 3 | H1, H10, H11, H12; gap list cut |
| Revision 4 | H9, H16 |
| Revision 5 | H7, H14, H25; "The twin report keeps skips apart" and the gap list cut |
| Revision 6 | H4, H26; the gap list cut |
| Revision 7 | H16; gap list cut |
| Revision 8 | H2, H3; "Task plan" and the gap list cut |
| Revision 9 | H2; gap list cut |
| Review records | cut (logs) |
| Drain after the thread-prep run | H4, H16, H19, H21, H22, H23, H25, H26; its list of marks cut |
| Kept minors from the thread-prep run | 1.1 to 1.9 H13; 2.1 to 2.7 H17; 2.8 to 2.10 and 4.8 H18; 4.4 to 4.6 H9; 4.7 H20; 5.6 to 5.8 H12; 6.1, 6.2 H15; 6.3, 6.4 H22; 6.9 H24; 7.2 H19; 7.3 H23; the rest cut |
| Native-fidelity review after the minors | first point H14; the rest cut |
| Compaction (2026-10-07) | cut (log) |
| Report drain (2026-10-07) | cut (routine fixes) |
| Walkthrough reports | cut (routine fixes) |
| Options table | cut (log; options.md holds the rows) |
| Runner directives | cut (log; §13.3 holds the directives) |

## 6. Cuts and moves across sets

Cuts. Each restatement is held by its owner's current text, and its owner's plan keeps it there, both checked while writing this plan. Where the owner's plan renumbers a section, both numbers are given.

- The preamble's list of lane 1's contents: THREAD.md Execution, lane 1.
- Old §3's naming rule for file-local helpers under unified sources: SPEC-integrator.md R-ALL-8 (old §7.4, new §12).
- F24's account of where DFG and FTL plans fail for lack of executable memory (the `FailedFinalizer` sites in `SpeculativeJIT::compile` and `compileFunction`, the two paths that set `FTL::State::allocationFailed`, Bun's FFI invoke thunk among them, and `FTL::fail`): SPEC-integrator.md §2, N9, which the integrator plan keeps whole. The baseline sentence stays in F24, since N3 does not hold its "only"s.
- Old §8.1's paragraph on a root UFE's own singleton bit, its readers and costs: THREAD.md Restoration (its evidence moves to H14).
- Old §8.2 rule 1's aside that the CB lane bounds its copies by `SpecBytecodeTop`: SPEC-cb.md §3.4, V6.
- Old §8.6's account of the polymorphic rule (`hasPolymorphicSite`, no counter progress): THREAD.md Restoration, SPEC-ics.md §5.3, SPEC-cb.md §4.3 (its argument moves to H16).
- Old §10.1's enumeration of the index spaces: SPEC-image.md §12.2, R-UCB-1, with SPEC-cb.md §6.2, R-UCB-2, and SPEC-ics.md §8.3, R-UCB-1.
- Old R-INT-3's account of how the lookup is met: SPEC-integrator.md old §7.2 (new §6.2), and SPEC-integrator.container.md sections 4.5 (what the integrity and structure checks are), 6 and 7.
- Old R-INT-11, all but the requirement it keeps (section 2, "§9 Interfaces"), part by part:
  - the placement of both calls, the `DFG::Plan` flag with the four branches where the compiling thread sets it, the worklist lock that orders the flag's write before its read, and that both calls run inside the plan's finalization with no worklist lock held: SPEC-integrator.md old §10 (new §9), with N9 for the branches;
  - the baseline case's reason, that a baseline plan fails only for lack of executable memory: old §10 (new §9)'s baseline edit, whose comment cites N3 and F24;
  - the entry point's signature, plan-site enumerators and contract (VM thread, no cell allocated, no stop for the collector, no wait on a thread): SPEC-integrator.md old §5.4 (new §4.5);
  - the GC deferrals the plan sites run under (the `DeferGC` of `completeAllReadyPlansForVM`, the deferral a synchronous route's caller holds): SPEC-integrator.md old §12 (new §10), whose GC paragraph takes them from old §10, with N8;
  - that Bun's `vm.Script` route reaches `BaselineJITPlan::finalize` through `JIT::compileSync`: SPEC-integrator.md N2 and old §12 (new §10)'s one statement of that route.
- Old §10.2's closing paragraph, sentence by sentence:
  - which part calls `didFailExecutableAllocation` at which sites: THREAD.md Execution, and SPEC-integrator.md old §5.4 (new §4.5)'s site table;
  - that the list is exhaustive, with Yarr, Bun's FFI thunks and stubs and WebAssembly keeping native behavior: THREAD.md Failures. The clause that those sites write nothing a capture reads is the evidence for that list and moves to H26 inside this set;
  - that an FFI invoke thunk an FTL plan cannot allocate still reaches the plan site, through `FTL::State::allocationFailed`: SPEC-integrator.md old §10 (new §9)'s FFI paragraph, which the integrator plan makes the one place for that case, and N9, which holds the mechanism.
- Old §12's second sentence, on how required values are captured: SPEC-integrator.md old §6.3 (new §5.1) and options.md's fixed table.
- Old §13's opening pointer to THREAD Verification, its producer-then-consumer sequence and its account of the oracle (which runs are compared with a JITCache-off run, and on what): SPEC-integrator.harness.md §7.3, §7.4 and §7.6, and THREAD.md Verification. Its build matrix is not cut (G7). The two passages that still name the oracle comparison cite harness §7.6 for it: §13's introduction, and §13.3's convention sentence, which cited §13 before the cut.
- Old §13.3's convention bullets 1, 2 and 4, and bullet 5's list of twins-only helpers with its rule to assert on them only where they exist: SPEC-integrator.md R-ALL-4 (old §7.4, new §12) and SPEC-integrator.harness.md §5.2 to §5.4, §7.2, §7.3 and §7.7. Bullet 5's release clause and twins declaration move into §13's introduction.
- `atom-constants.js`'s clause turning `useConcurrentJIT` off in every run: the table of SPEC-integrator.harness.md §7.4, which the integrator plan keeps, and §7.7.
- The stress line's "with `--destroy-vm`": SPEC-integrator.harness.md §7.3 for jsc runs and §7.7 (`BUN_DESTRUCT_VM_ON_EXIT=1`) for Bun runs. "Every debug run ... ASan and LSan clean" stays.
- B2's list of the `request` event's fields (the key, what the request point did, each part's time and the two totals): the `request` row of SPEC-integrator.harness.md §9.2, which the integrator plan keeps. That row calls each part's time CPU time; B2's clock sentence, which says each part reads `CLOCK_MONOTONIC`, is not cut (section 2, "§14 Bench obligations"; G9).
- B2's clause that IB3 joins the `request` event with the `install` event by key: SPEC-integrator.md IB3 (old §17, new §16), which the integrator plan keeps.
- The accounting paragraph's restatement of what THREAD's bound counts: THREAD.md's opening.
- Old §5.2's list of when the install glue calls `pendingImport` and `resolvePendingImport`, with its last bullet, that the glue calls neither after a baked-fact mismatch, which keeps the import for the next newborn CodeBlock, nor after invalid material, which leaves it to die with its UCB: SPEC-integrator.md old §8.2 (new §7.2), steps 1, 5, 6 and 17, and old §8.3 (new §7.3)'s outcome table.

Move. X1: the `Off`-role convention of old §13.3's third bullet (under `Off` a script runs its Consumer path without `delta` and without any assertion about imported, seeded, attached or captured state; assertions about native behavior, such as results, a thrown error or `cachedDataRejected`, run in every role) belongs in SPEC-integrator.md R-ALL-4, beside the conventions it already holds for every part; SPEC-ics.md §11.2, SPEC-cb.md §11.3 and SPEC-image.md old §17.1 (new §16.1) restate it too. SPEC-integrator.md's plan places it there (its new section 12, R-ALL-4) as one sentence holding the whole bullet except "without statistics". §13.3 therefore keeps only that lane clause, naming `$vm.jitCacheUCBStatistics()`, and cites R-ALL-4. If R-ALL-4 does not hold the sentence when this set is rewritten, the bullet stays whole, so nothing is lost.

## 7. Contradictions and gaps, left unfixed

- G1 (THREAD). THREAD's opening lists the omissions and says a SPEC cannot add one. Old §6.2.2 (§7.2.2) adds one: when `decodeCachedCodeBlocks` decodes both slots of a UFE, the slot nobody requested never receives its UCB feedback (arithmetic profiles, exit sites, quick tier-up bits, `didOptimize`, the LLInt counter, its children's singleton bits). The SPEC rests it on THREAD Session's "no work ahead of demand" and Identity's rule that a UCB is seeded only before publication, and both slots are published together. THREAD must list this omission or choose otherwise.
- G2 (THREAD, minor). THREAD Session charges every byte JITCache owns for production and exempts the parent-key registry because it serves every role. Two registry fields serve production only and go uncharged: `UCBRecord::generatedButterflyMap`, kept only while production is active for a later capture, and a direct eval record's context digest, computed at the record in a producing VM for capture. THREAD should say whether its exemption covers them.
- G3 (codec, minor). Codec §2 builds the descriptor with "a 16-byte header with magic `0x44424355` and `rootOffset`" without the field offsets or the other bytes, which codec §3 gives in full for the core payload. Producer and consumer share a build, so any fixed layout works, but the implementer has to choose one.
- G4 (stale, harness). Old §13.3's first convention bullet passes three values, `main(role, scratch, artifact)` and `process.argv[2]` to `[4]`, while harness §7.3 and §7.7 pass four, with the sequence index (`arguments[0]` to `[3]`, `process.argv[2]` to `[5]`), and `start-line.js` and `invalid-material.js` pick their case by that index. The plan's cut leaves the harness as the only statement.
- G5 (stale, integrator). Old §12 says the integrator's table captures each required value at build time from `FOR_EACH_JSC_OPTION`; SPEC-integrator.md old §6.3 (new §5.1) writes the values from options.md into a macro list and checks them against the defaults with T-OPT. The plan's cut leaves that section as the only statement.
- G6 (wording). THREAD Restoration arms the LLInt counter "without a slice", while old §8.1's table says "the slice is armed anew" and options.md speaks of "the LLInt counter's slice, which the request point arms anew". Native `ExecutionCounter::setThreshold` with no CodeBlock still clips to the checkpoint ceiling (`clippedThreshold`) and applies no memory-pressure correction, so THREAD's "slice" means the memory-pressure slice; the decisions agree. The plan words §5.1 and §5.5 in THREAD's sense; options.md's wording is outside this set.
- G7 (contradiction, HARNESS.md and integrator). The lane runs its tests in a release build that no other file schedules. Old §13 runs every JS test that does not declare `// jitcache-requires: twins` "in a release build without" twins, old §13.3's fifth convention bullet says such a script "also runs in the release build, where the oracle comparison is its check", R-INT-10 asks for the runner "in every build" for that reason, and `compile-function.js`'s `cachedData` case "calls the user function only in release runs", since ASan reports F11's use-after-free with JITCache off as well. HARNESS.md ("Tests") and SPEC-integrator.md old §16 (new §15) run tests in the twins build and the plain `debug-local` build, both with ASan and LSan; HARNESS.md uses `release-local` for benches only, and its cadence never schedules a release test run; harness §7.1 runs whatever build `--build` names and fixes no build type. Under HARNESS.md no run is a release run, so the `cachedData` case never calls the user function, and a script that is not twins-only also runs in a plain debug build, which the lane's matrix does not list. SPEC-image.md old §17.1 (new §16.1) has the same split (the image plan's gap 3). The plan keeps the matrix, the release clause, R-INT-10's "in every build" and the `compile-function.js` row as they are; which builds run the lane's tests is for the human.
- G8 (stale, integrator, minor). Old R-INT-11's first bullet sends Bun's `vm.Script` route to the same `BaselineJITPlan::finalize` through `JIT::compileSync`, and its third bullet says both plan-site calls run "under the `DeferGC` of `JITWorklist::completeAllReadyPlansForVM` or the deferral of a synchronous route". SPEC-integrator.md old §10, whose sentences the integrator plan moves to its new §10, says that route calls `JIT::compileSync` after Bun's own `DeferGC` has closed, so its finalization runs with no deferral, as install.md ("Embedder routes") also says, and that neither the fault entry point nor the finalize hook assumes one. The decision is the same either way, since the entry point is safe inside any finalization; the plan's cut leaves the integrator's statement as the only one.
- G9 (contradiction, harness, minor; the integrator plan's G10). Harness §9.2's `request` row records "the CPU time of each part B2 lists". Harness §9.1 reads the thread CPU clock only where a counted span begins or ends and has a breakdown of a counted span into steps read `CLOCK_MONOTONIC`, with the install function's single span as its example; B2 applies that rule to the `request` event, so each part reads `CLOCK_MONOTONIC`. The integrator plan keeps both harness passages as written, so B2's last sentence stays whole: without it, the event's own row would name CPU time for each part. Making that row name the clock B2 and harness §9.1 give is for the human.
