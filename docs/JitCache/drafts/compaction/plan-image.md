# Compaction plan: the image set

The set is `docs/JitCache/specs/SPEC-image.md`, its binding census `SPEC-image.sites.md` and the record `SPEC-image-history.md`. This plan rests on reading the three whole, THREAD.md, skills/SKILL.md with its required references, options.md and HARNESS.md; in the other sets, every passage this set restates or that cites this set (SPEC-integrator.md, SPEC-integrator.harness.md, SPEC-integrator.container.md, SPEC-ucb.md section 10.1, SPEC-ics.md R-IMG-1 and R-INT-4, and the histories that cite SPEC-image.md); and the plans of the other four sets, so that no rule is cut on both sides of a border (section 7).

Keep means the passage stays in its section, possibly with fewer words; the section may get a new number. Move means it goes to another section of this set. Merge means it joins a passage that says the same thing, which becomes its one home. Cut means it leaves the set, and the named owner holds it in its current text. Section numbers are the old ones unless marked "new".

Rule IDs keep their names, and each stays with its section, so its new place is its section's new number. R10 is the one ID that stops existing: it states I1's property and merges into I1, and no file outside this set cites it. Two groups of names occur twice in the set and cannot be renamed: B1 to B5 name the bench obligations of old 17.3 and census rows of `SPEC-image.sites.md`, and C1 to C5 name the census rules of section 4.6 and census rows. The writer keeps every citation of them qualified by kind or file, as the census already does ("rule Cn", "census row B4").

Rules for the writer: retarget every section citation inside the set with the maps of section 6; cite code by symbol; keep every requirement this plan does not name as moved, merged or cut; tighten wording elsewhere only where no requirement, symbol or reason changes; the history cites no review round, batch number or date. Citations into other sets, THREAD, HARNESS.md and options.md stay as they are; a later pass retargets them.

## 1. The order of the reasoning

The SPEC should read as one argument. It says what the lane owns and which native facts constrain it. It then defines what a reference is and how a target names its object, how recording emits and records references, what the record holds, and the two features with a life of their own (MathICs and baked facts). After that come the bytes a body carries, how capture writes them, how preparation turns them back into a `BaselineJITCode`, and how test builds check the result. The lists that other parts and the implementer work from close the file: interfaces, failures, edits, invariants, tests and tasks.

Two forward dependencies break that order today.

1. Table 3.3 and the helpers of section 4.3 use `CodeSymbol`, `BaselineThunk`, `ProcessThunk`, `VMAddress`, `VMCell`, `resolveTarget` and `resolveSupport`, which section 8 defines four sections later, while section 8 depends only on section 3. Section 8 moves into section 3, right after the target kinds, so every name table 3.3 uses is defined next to it.
2. The twins material is spread over sections 4.2, 5, 6.2, 9.6, 10, 11.3 and 17.2, and the layout of `image-twins.baseline` (9.6) comes eight sections before the data it encodes (17.2). Section 17.2, section 9.6 and the regeneration-log paragraph of 6.2 become one top-level section right after preparation at install, where the check runs. The production sections keep their twins-only declarations and steps, and cite it.

The rest of the order holds. Placing the recording rules at the end of section 3 (new 3.9) puts them right before section 4 without renumbering section 4, which other sets cite, and placing the twins section at new 11 keeps sections 12 and 13 at their numbers.

New outline, with old numbers in parentheses:

1. Scope (1, plus the first sentence of 14)
2. Native facts (2)
3. The reference model: 3.1 Terms (3.1); 3.2 Forms (3.2, with the field positions of 9.4); 3.3 Target kinds (3.3); 3.4 Support keys (8.1); 3.5 Code symbols (8.2); 3.6 VM data (8.4); 3.7 String switch ranks (3.5); 3.8 Resolution (8.3); 3.9 Recording rules (3.4)
4. Recording: 4.1 to 4.8 (4.1 to 4.8; 4.1 also takes the regeneration condition of 6.2)
5. The image record (5)
6. MathICs: 6.1 to 6.4 (6.1 to 6.4)
7. Baked facts (7)
8. Sections: 8.1 Encoding (9.1, plus the CPU-feature sentence of 14); 8.2 `image.baseline` (9.2); 8.3 `baked-facts.baseline` (9.3); 8.4 Canonical bytes (9.4); 8.5 Validation (9.5, plus the `ImageCheck` declaration of 10)
9. Capture (10)
10. Preparation at install: 10.1 Interface (11.1); 10.2 Context (11.2); 10.3 Steps (11.3)
11. Twins: 11.1 Twin data (17.2's data part, 6.2's log paragraph); 11.2 The `image-twins.baseline` section (9.6); 11.3 The check (17.2's preconditions, declarations and steps); 11.4 The comparison (17.2's comparison, relocation clause and domains)
12. Interfaces: 12.1, 12.2 (12.1, 12.2)
13. Failures (13)
14. Native edits and owned paths: 14.1 to 14.4 (15.1 to 15.4)
15. Invariants (16)
16. Tests and bench: 16.1 Test obligations (17.1); 16.2 Bench obligations (17.3)
17. Tasks (18)

Within sections the order changes in five places. Section 3.2 gives the forms table, then one table of canonical bytes, then the one reason canonical footprints matter. Section 4.1 states when a compilation records, when a regeneration records, what follows from both, and then the table of reasons. Section 6.2 goes from why the function moves, to its declarations, the activity rule, `MathICRegeneration` and edits 1 to 5. Section 8.5 states the split between normal mode and strict once, then declares `ImageCheck`, then lists the V and U checks. Section 11 goes from the data twins record, to the section that carries it, to the check, to the comparison.

## 2. SPEC-image.md, passage by passage

### Preamble

Keep the lane line, the sub-SPEC bullet (its citations of 4.3 and 4.6 stay) and the conventions. The history line becomes: [SPEC-image-history.md](SPEC-image-history.md) records why the decisions it names are what they are, for whoever revisits one; nothing in it binds, and implementing needs none of it.

### Section 1, Scope

Keep items 1 to 7. Add item 8 from section 14: the lane's rows in options.md, which are the fixed options that change the image's bytes or its references, the rows THREAD lists that the lane relies on, and the free options baseline emission reads; `start` checks the fixed ones (R-INT-9).

### Section 2, Native facts

- Cut the opening sentence ("Each fact was verified in the code, and the design depends on all of them."). It is provenance, which SKILL.md's "Writing SPECs" leaves out.
- N1: its last sentence ("R3's helpers replace both shapes.") moves into R3 as the citation "(N1)".
- N15 absorbs N29's sentence on the three callers of `VM::setShouldBuildPCToCodeOriginMapping`, so N15 is the one fact about PC-to-origin maps. options.md cites N15 for the maps and N29 for Debugger-mode bytecode, the ShadowChicken opcodes and the per-bytecode profiler, and every one of those citations stays true.
- N17 stops pointing at section 6.1 and states the native trace now in 6.1, with its examples: the two cases in which `JITMathIC::generateInline` returns false (`DontGenerate`, as at a `+` that only saw strings or a `*` that only saw BigInts; `GenerateFullSnippet` with a `generateFastPath` that refuses on `OperandTypes`, as at `"x" + y`), the direct operation call in `JIT::emitMathICFast` with no slow case, `emitSlow_op_*` and `JIT::emitMathICSlow` never running for that bytecode, the four null locations, no `m_code`, `m_generateFastPathOnRepatch` false and no slow call; and that every path on which `generateInline` returns true appends a slow-path jump, so the IC gets all four locations. Section 6.1 keeps the design.
- N26: the sentence on `ImageRecorder.cpp`'s `static_assert`s moves to section 3.3, after the table: it is an edit of this lane, not a native fact.
- N29 loses the PC-map sentence that N15 now holds.
- The other facts keep their text; the writer may tighten wording but keeps every symbol a fact names.

### Section 3, The reference model

- 3.1: keep.
- 3.2: drop the "canonical encoding" column of the forms table, which the second table states exactly, and give the second table the zeroed fields that 9.4 lists in prose (x86_64: the eight immediate bytes of a `Pointer` and the `rel32` of a `Call` or `Jump`; ARM64: the `imm16` field, bits 5 to 20, of each of a `Pointer`'s three words, and the `imm26` field, bits 0 to 25, of `bl` and `b`; opcodes and `Xd` kept). In the paragraph after the tables keep the first sentence, the reason canonical footprints matter (N21: a `nop` at site 0 sends ARM64's `linkJump` before the allocation). Replace the rest, which restates the checksum, normal mode's trust and V3, with one sentence: capture writes every footprint in its canonical encoding (8.4, I8), and section 8.5 says what checks it. Link the history record "Forms and canonical footprints".
- 3.3: keep; add N26's `static_assert` sentence after the table. Retarget "section 8.3" to 3.8, "table 8.4" to 3.6, "section 3.5" to 3.7, "section 9.5" to 8.5.
- 8.1 becomes 3.4, unchanged except its citations; link "Code symbols and the thunk enumeration".
- 8.2 becomes 3.5, unchanged except its citations.
- 8.4 becomes 3.6, unchanged except "section 9.2" to 8.2; table 8.4 is table 3.6 everywhere.
- 3.5 becomes 3.7, unchanged except "section 8.3" to 3.8 and "section 11.3, step 7" to 10.3; link "String switch ranks".
- 8.3 becomes 3.8, unchanged except its citations. Its paragraph after the declarations keeps the list of checks that validate targets (V3, V5, V6, U3, U6 and U7), which the walkthrough completed, and the clause that normal mode resolves without checking.
- 3.4 becomes 3.9, with R1 to R9. Its lead says the rules keep every byte outside a fixup the same in every process (I1). R3 gains "(N1)". R4 links "Cached temp registers". R6's last sentence (the native link once a recorder stops) becomes a citation of 4.5 and I19, where that rule lives. R10 merges into I1: both state that two compilations with the same inputs and draws differ only inside footprints, and the invariants are where this SPEC states properties.

### Section 4, Recording

- 4.1: keep its first paragraph, including the sentence that a UCB gets its record only at a request point, before it is published (SPEC-ucb.md section 5.4), which SPEC-integrator.md section 5.3 now cites instead of restating. It absorbs from 6.2 the full condition for a regeneration to record (a producing context, a CB whose JIT type is `BaselineJIT`, a `Complete` record; imported images in a ConsumerProducer VM; a twin regeneration always records into its twin compile's record, section 11.3), replacing old 4.1's second paragraph. Order: when a compilation records, when a regeneration records, what follows from both, then the table of reasons, whose names do not change. Link "When a compilation records".
- 4.2: keep; the twins sentence cites 11.1.
- 4.3: keep. In the hooks paragraph cut the list "(A2, A4, A5, A10, A12, A13 and A18)" and cite the census preamble, which holds that list. Link "Finding references".
- 4.4: keep; "section 17.2" becomes 11.4. Link "Finding references".
- 4.5: keep; link "ARM64 veneers".
- 4.6: keep.
- 4.7: keep; step 6 keeps its action and cites 4.8 for the release rule. Link "The linked size" at step 2.
- 4.8: keep. Its closing sentence keeps "this lane raises nothing itself" and cites R-INT-2 for the budget's refusal and its fault, instead of restating them. Link "Charging".

### Section 5, The image record

Keep. The twins sentence cites 11.1, and "section 17.2" in the `BaselineJITCode` paragraph becomes 11.3.

### Section 6, MathICs

- 6.1: shrinks to the design. `emit_op_add` and its siblings call `noteMathIC` right after `addJIT*IC`, so the record and the holder count the same ICs (S3); a MathIC has inline code or none (N17); `finishBaselineCompile` reads each noted IC's locations after the link tasks and classifies them in three cases; the locations, the flag and `m_code` are read at capture. The native trace moves to N17. Link "MathICs without inline code".
- 6.2: keep the reason `generateOutOfLine` moves to `jit/JITMathIC.cpp`, its two declarations, the wrapper, `MathICRegeneration`, edits 1 to 5 and `didLinkSnippet`. The "active only when" paragraph keeps the programming-error rule for an IC the record does not list and the inactive and twin behaviour, and cites 4.1 for the condition. `didLinkSnippet`'s release sentence cites 4.8. The twins regeneration-log paragraph ("In twins builds an active native regeneration also appends its entry...") moves to new 11.1. Links: "Exported headers" at the move, "The inline-start rewrite" at edit 1, "The MathIC allocation fault" at edit 5.
- 6.3: keep.
- 6.4: keep; link "The negate operations".

### Section 7, Baked facts

Keep; link "Baked facts".

### Section 9, Sections (new 8)

- 9.1 becomes 8.1 and gains, from section 14, that the header's CPU feature vector fixes the instruction forms emission chose (THREAD Storage), beside the build ID's role.
- 9.2 becomes 8.2: keep; link "Side tables and section headers".
- 9.3 becomes 8.3: keep.
- 9.4 becomes 8.4: keep its rule (capture writes the live bytes and, inside each footprint, the canonical encoding of 3.2; sections are address-independent and repeatable, I8); the bit positions go to 3.2.
- 9.5 becomes 8.5. It receives the `ImageCheck` enumeration and `description(ImageCheck)` from section 10's code block, since 8.5 is where the checks they name are first defined; `CaptureOutcome` and `CaptureFailure` stay in capture, and the enumeration's comments retarget to 8.5, 11.2, 9 and 10.3. Its first paragraph is the one statement of the normal-mode and strict split for this lane, which 3.2, 3.8 and 10.3 cite. V6 cites table 3.3 for the conditions of `MathIC` and `SwitchTableBase` targets and keeps its reason (only `JIT::emitMathICSlow` passes an IC's address). U5 keeps the count rule and replaces its second sentence, which restates N12, with "(N12)". U6 keeps the per-rank completeness (one case fixup per rank, at least one atom fixup per rank) and cites table 3.3 for the bounds. U7 cites table 3.3's conditions for `UCBConstantCell` and `UCBConstantAtom` instead of restating them. The closing sentence on W1 to W4 cites 11.2. Links: "Validation", and "Check names" at `ImageCheck`.
- 9.6 moves to new 11.2.

### Section 10, Capture (new 9)

Keep, without the `ImageCheck` declaration (now in 8.5). Step 6 cites 11.1 and 11.2. Link "Capture".

### Section 11, Preparation at install (new 10)

- 11.1 becomes 10.1. Keep the code block. Cut the paragraph that lists the install glue's order (parse, validate, compare, gate, prepare, the other preparations, the seeds, commit, setup, the IC attachment, the counter and `installCode`): SPEC-integrator.md section 8.2, steps 3 to 16, holds it, with II12. In its place one sentence: the glue calls these in THREAD Restoration's order under R-INT-7, and `prepareImage` and `commit` keep I11. The paragraph on `code()` stays as the one statement of its contract and takes from 12.1's ICs row that the molds carry the producer's fields, so SPEC-ics.md R-IMG-1, which its plan trims to one sentence citing 11.1 and 12.1, still finds the whole contract here. Link "Preparation and the rebuilt record".
- 11.2 becomes 10.2: keep.
- 11.3 becomes 10.3. Step 1 stays, so steps 7, 10, 13 and 14 keep the numbers this set and SPEC-integrator.md cite, and shrinks to: validation is the glue's, under strict and before `compareBakedFacts` (R-INT-3, R-INT-7, section 8.5); either way every footprint holds its canonical encoding (I8), which the writers of step 9 rely on. Step 14's twins part cites 11.1. In the closing paragraph cut the first sentence, which restates what native setup publishes, retargets, builds and parks (N12 holds what setup reads; SPEC-integrator.md section 8.2, step 13, calls setup), and keep "From then on the image is native: ..." reworded to stand alone ("Once native setup installs it, the image is native: ..."). Link "Validation" at step 1 and "Preparation and the rebuilt record" at step 14.

### Section 12, Interfaces

12.1 keeps its table. The ICs row names `PreparedImage::code()` as the argument of the ICs lane's `prepareBaselineICs` and points to 10.1 for its contract instead of restating it; the status row points to 8.5 for `ImageCheck` and keeps naming `description(ImageCheck)`, which SPEC-integrator.md section 5.4 cites through 12.1; the option-table row points to section 1 and R-INT-9; the harness row to 11.3.

12.2, requirement by requirement:

- R-UCB-1: keep the list of index spaces, which SPEC-ucb.md's plan now cites instead of repeating, and "The image section records none of these, so no U check compares them". Cut how the UCB lane guarantees them under strict ("under strict the UCB lane's check of a reused UCB against the digest of the captured core covers them, and an imported UCB is that core") and the details of the atom contract (which atoms the contract covers and why, that extra atoms change no restored byte, that the twin takes the producer's choice): SPEC-ucb.md section 10.1 holds them in its index-space and string-constants paragraphs. Keep one sentence: beyond the index spaces the lane relies only on THREAD Restoration's atom contract, which the UCB lane keeps (SPEC-ucb.md section 10.1), for the constant of each `UCBConstantAtom` fixup and of each strict-equality input that names an operand (11.1, N25).
- R-UCB-2, R-UCB-3, R-UCB-4 and R-ALL-1: keep.
- R-ICS-1: keep. SPEC-ics.md's plan cites it at its sections 4.2 (mold order) and 6.4 (attach by mold index).
- R-INT-1: keep the signature; non-null exactly while the VM's production is active (a producing role, cache activity on, production not ended); the same context each time until it returns null for good when production ends (THREAD Session and Failures); callable from any thread without a lock. Cut the list of what ends production and turns cache activity off (a recording fault; invalid material, an executable-allocation fault, a debugger attach) and the case of a debugger attached before `start`: SPEC-integrator.md sections 5.2 (the switch table), 5.3 (`producerContext`) and 4.2 (step 5) hold them. Cut "A compilation that read it just before it turned null may still record", which 4.1 states.
- R-INT-2: keep as written. Sections 4.8, 9, 10.3 (step 14) and 13 cite it for the budget raising the recording fault, so its THREAD semantics stay here.
- R-INT-3, R-INT-5, R-INT-6, R-INT-8 and R-INT-10: keep.
- R-INT-4: keep the signature, the site `ExecutableAllocationSite::MathICSnippet` and this lane's call context (VM thread, API lock, no `CodeBlock::m_lock`, inside the repatching operation). Cut the restated contract (any JSC lock held, allocates and frees nothing, takes no lock, waits for no thread, a no-op without `start`, the step name), which SPEC-integrator.md section 5.4 holds in its declaration comment and site table, and cite it.
- R-INT-7: keep; "the order of section 11.1" becomes "THREAD Restoration's order, which SPEC-integrator.md section 8.2 spells out".
- R-INT-9: keep; it now carries what section 14 asked of `start`.
- R-INT-11: keep the ID and every requirement, as a list whose items cite their owners:
  - twins builds compile and link with Bun's build flags like every other build, so none is position-independent, and the relocation clause skips the targets those flags pin (THREAD Verification, 11.4);
  - a per-VM `JITCache::Twins`, created no later than the VM's first `Twins::checkImage` and destroyed during VM destruction, at any point, since it holds no twin CB (11.3, step 2). SPEC-integrator.md's plan cuts this reason from harness section 3.3 and cites this item and 11.3, so it stays;
  - the `image-twins.baseline` span passed to `parseImageSections` in `ImageSectionSpans`, and the payload kept alive until `Twins::checkImage` returns;
  - a call to `Twins::checkImage` for every installed import, at the end of `ScriptExecutable::prepareForExecutionImpl` after `installCode` and before JS runs, on both install points (before `setupLLInt`, and in `JIT::compileSync` reached through `setupJIT`), with the installed CB, the scope `prepareForExecutionImpl` received, the committed `BaselineJITCode` and the parsed sections;
  - the producer-then-consumer runner: strict on in every process (THREAD Verification); `useConcurrentJIT` off "in every process of a twins run", words SPEC-integrator.harness.md section 7.4 quotes; a skipped image check failing a run, as a difference does, under THREAD Verification's skip rule (11.3);
  - the runner moving every relocation domain 11.4 compares between each process that imports a body and each process whose captures it imports, a ConsumerProducer that recaptures a body being that body's capturing process for every later importer (T15): the importing process's executable pool in a range disjoint from the capturing process's and its structure reservation at another base, both settled before its `JSC::initialize` makes them (N22), for example with an inaccessible placeholder mapping over every free page of the capturing process's two ranges or, for the pool, with `jitMemoryReservationAddress`, a free option (options.md cites R-INT-11 for these two methods); address randomization on in every process (`kernel.randomize_va_space` at 2, no `ADDR_NO_RANDOMIZE` personality), which moves the heap; a check on each architecture that the heap moved, and, where the build's allocator keeps a fixed base, the heap taken from an allocator whose mappings the kernel places;
  - a run whose only twin reports are equal relocation pairs in the heap domain repeated once in fresh processes, an equal pair in the repetition failing it, with the reason moved up from the history (section 4.1 of this plan);
  - a twins-only run flag, `--jitcache-test-image-hook=<name>` with `<name>` one of `relocation-pairs`, `operation-pair`, `change-recorded-target` and `skip-patch`, that sets the process's image test hook (16.1) through `JITCache::setImageTestHook` before its first VM, in each host (SPEC-integrator.harness.md section 5.1; SPEC-integrator.md section 14.2);
  - the C++ unit-test runner for `Source/JavaScriptCore/jitcache/tests/`, and a `TwinReport` sink that keeps a skipped check, with its reason, apart from a difference.

  Cut: the parenthetical on how the integrator creates `Twins` at the VM's first stash (SPEC-integrator.harness.md section 3.3 holds it); the hook flag's host mechanics, with the stale "and the matching Bun environment variable" (SPEC-integrator.harness.md section 5.1 and SPEC-integrator.md sections 4.5 and 14.2 hold them; section 5 of this plan, item 1); the engine image's link-time address and the skip of its targets (N23, 11.4's second skip); the sentence that a self-captured body compares no pair, so placement and repetition concern only imports across processes (11.4's third skip); the last sentence, on runs that keep `useConcurrentJIT` on, which get a skip for every import and fail only on differences (SPEC-integrator.harness.md section 7.4 under THREAD Verification's skip rule; T14 states its own case). Link "Moving the domains".
- R-INT-12: keep as written. SPEC-integrator.md's plan cuts its own statement of the flag's semantics (written once before the plan is enqueued and read without a lock, one null test without a state, the twin's plan computing it the same way while its recorder ignores it) and cites R-INT-12 and 4.1, so both stay here.

### Section 13, Failures

Keep the table. In the row "`JIT::link` | no executable memory", replace the clause on the integrator raising the fault on the failure path of `BaselineJITPlan::finalize` with a citation of SPEC-integrator.md section 10, which holds it. The other rows stay.

### Section 14, Option table rows

Merged away: its first sentence becomes Scope item 8; "CPU features are the header's" goes to 8.1; R-INT-9 keeps the requirement. Cut "`start` checks the effective values after `Config::finalize`": SPEC-integrator.md sections 6.3 and 4.2 (step 4) hold it.

### Section 15, Native edits and owned paths (new 14)

- 15.1 becomes 14.1: keep the table. In the `jit/JIT.h` row, "so `JIT` gains no member that exists only in twins builds" cites 14.4. The paragraph after the table keeps only the definition of `BinarySwitchRankedComparisons` (an abstract class in `BinarySwitch.h` with the one virtual function and its signature); the rest restates 4.3's `StringSwitchRecording` rows and census rows C13 and A16, which hold it.
- 15.2 becomes 14.2: keep the file list, with retargeted section numbers (`ImageTwins.h` serves section 11). The unified-sources sentence shrinks to: file-local names follow SPEC-integrator.md R-ALL-8 with the prefix `image`, which R-ALL-8 already names.
- 15.3 becomes 14.3: keep M1 to M8. SPEC-integrator.md's plan cites M1 for this lane's `Sources.txt` files.
- 15.4 becomes 14.4: keep. In the last paragraph, cut the restated list of the headers Bun includes (SPEC-integrator.md section 4.5 and M2) and keep "no lane exports a header, this one included". Link "Exported headers".

### Section 16, Invariants (new 15)

Keep. I1 absorbs R10 with no change of wording. I21 keeps its first sentence and replaces the second with a citation of SPEC-integrator.md II18. I20 cites 11.3.

### Section 17, Tests and bench (new 16)

17.1 becomes 16.1:

- The first paragraph stays as it is; its build matrix differs from HARNESS.md and SPEC-integrator.md section 16 (section 5 of this plan, item 3).
- The paragraph on JS tests shrinks to its first clause: the JS tests follow SPEC-integrator.md R-ALL-4 and the runner's directives and argument conventions (harness sub-SPEC sections 7.2 to 7.6). The rest of the paragraph restates rules other sets hold, and is cut. R-ALL-4 and SPEC-integrator.harness.md sections 7.2, 7.3 and 7.6 hold the oracle's comparison, `(function main(role, scratch, artifact) { ... })(...arguments)` (the harness in its current four-argument form), the rules on role-independent output and heap, and `jitcache-heap: off`. R-ALL-4 holds the `Off`-role sentence once the integrator plan places the ucb plan's move X1 there (plan-integrator.md section 2, new 12, and section 8), and nothing in that sentence belongs to this lane alone. Its "the same program" is the Consumer path R-ALL-4 names: the oracle compares every role's output with the `Off` run's, so a script's roles differ only in work that prints nothing, such as T18's property-key uses and call orders. R-ALL-4's "imported, seeded, attached or captured" state covers its "imported or captured" state.
- The test-hooks paragraph stays and cites 11.3 for `ImageTestHook`.
- T1 to T21 stay. T2's parenthetical restating R-INT-11 becomes "driven by the runner of R-INT-11". Citations of 17.2 retarget to 11.1, 11.3 or 11.4 by what they name.

17.2 becomes section 11:

- 11.1, Twin data: 17.2's opening paragraph, "Seeds", "Compile inputs" with its list, the atom-ness paragraph, "Emission reads nothing else that changes", the regeneration-log paragraph of 6.2 (replacing 17.2's one-line pointer), the producer-values paragraph and the rebuilt record's twin data. The declarations `TwinSeeds`, `CompileInputKind`, `StrictEqualityAtomOperand`, `CompileInput`, `TwinCompileInputs`, `TwinRegeneration`, `TwinData`, `snapshotCompileInputs` and `captureProcessToken` come with it, as the first of the two code blocks 17.2's one block splits into. Link "The twin CB and its inputs".
- 11.2: old 9.6 whole, with W1 to W4.
- 11.3, The check: the preconditions paragraph through "`checkImage` tests both first (below), and the `TwinReport` keeps the skips it reports apart from differences (R-INT-11)", then that with `useConcurrentJIT` off in every process of a run both preconditions hold for every import, so a skip there is a defect and fails the run (THREAD Verification), and that twins runs turn the option off in every process (R-INT-11, T2). SPEC-integrator.md's plan cites this paragraph for when the image check skips, and SPEC-cb.md's plan for the skip whenever `useConcurrentJIT` is on, so both stay. Cut the sentence on a run that keeps the option on in an importing or capturing process (SPEC-integrator.harness.md section 7.4, under THREAD Verification's skip rule; T14) and "`useConcurrentJIT` is free under THREAD Storage's rule" (options.md's free row). Then the second code block: `TwinReplay`, `Twins` with its comment that it holds no state, the twin registry functions and the test hooks; then `checkImage`'s skip rule and steps 1 to 7, which keep their numbers. Step 3 gains the clause moved up from the history (section 4.1). Links: "When the twin check runs", "The twin's lifetime" at step 2.
- 11.4, The comparison: the paragraph on two allocations, the bullets (sizes and bytes, fixups, decodes, tables and states, rates, relocation), "Each difference goes to the `TwinReport`", the domain paragraph and the table. The relocation clause's second skip keeps its zero-load-bias predicate and cites the table's engine-image row for the targets it skips, instead of listing them a second time. The heap row keeps "address randomization of the space the heap's allocator maps" and its citation of R-INT-11 for the runner's check, its fallback and the repetition, which R-INT-11 keeps. Links: "Comparing an image with its twin", "Relocation domains", "Bodies a process captured itself".

17.3 becomes 16.2: keep. B1's "Like every bench obligation, it runs with strict at its default, off (THREAD Verification)" becomes "It runs with strict off (THREAD Verification)".

### Section 18, Tasks (new 17)

Keep, with tasks 1 to 13 at their numbers; retarget section numbers (for example task 1's "section 9.5" to 8.5, task 10's "section 11.3, step 2" to 10.3, task 11's "section 10, step 6", "section 11.3, step 14" and "section 17.1" to 9, 10.3 and 16.1).

## 3. SPEC-image.sites.md

- Preamble: "its table 8.4" becomes "its table 3.6", and "SPEC-image sections 4.3 and 15.4" becomes "4.3 and 14.4". The list of rows whose sites reach recording through hooks stays here, as the one place for it (4.3 cites it), and so does the "rule Cn" convention.
- Rows A1 to H keep their IDs and text, A7a, A11a and A15a included. Retarget "SPEC-image section 9.2" in the E note to 8.2 and "SPEC-image section 17.2" in the C6 and C7 note to 11.1; the citations of sections 4.3, 4.4, 4.7, 6.1, 6.2 and 7 keep their numbers. The E note stays whole: SPEC-ics.md's plan cuts its own sentence on who edits `JIT::compileOpCall` and cites section E.
- C20 note: "an option that changes parsing and so stays fixed off under THREAD Storage's rule" becomes "which options.md fixes off".
- F note: "A MathIC whose `generateInline` returns false emits only F2's operation call. Its bytecode leaves no slow case, so F3 and F4 never run for it, and the IC keeps no locations and no slow call (SPEC-image section 6.1)." becomes "A MathIC without inline code emits only F2's operation call, and F3 and F4 never run for it (SPEC-image N17)", keeping "F1 still notes it".
- D4 note: keep; link the history record "The scope thunk key".

## 4. The history

### 4.1 What moves up into the SPEC

Two clauses, each a reason an implementer would otherwise get wrong.

1. From revision 2, "The twins producer side", into new 11.3, step 3: the GC reaches a `ProtoLoad` entry only through `m_llintGetByIdWatchpointMap`, which is empty for the twin. Step 3 now argues the safety of its `get_by_id` writes from the `Default` entry alone, which leaves `ProtoLoad` entries unexplained.
2. From revision 3, "The heap", into R-INT-11's repetition item: a heap coincidence is chance, which fresh processes do not repeat, while a defect such as a resolution that ignores its context recurs. HARNESS.md forbids rerunning a failed test and SPEC-integrator.harness.md section 7.5 states this one exception without a reason, so R-INT-11, where the requirement starts, is the place for it.

Every other reason the implementer needs already sits in the SPEC beside its decision.

### 4.2 The records the history keeps

The history becomes one record per non-obvious decision, in SPEC order, each saying what first seemed right, the evidence that changed it, and what the SPEC does now. Records are headed by title, which is how the SPEC and other files link them. Its header says that it records why the decisions are what they are, for whoever revisits one, and that nothing in it binds. Below, each record gives its sources in the current history and a sketch of its content.

1. Finding references. Sources: draft 1, "Why explicit helpers instead of tagging `TrustedImmPtr` and `AbsoluteAddress`" and "Alternatives considered and rejected". First: carry a target inside the immediate types, or find references after emission (scan the linked bytes, classify immediates by value, ship pre-compaction code, recompile in the consumer). Evidence: tagging reaches hundreds of `MacroAssembler` operations on two architectures; blinding, folding and compaction make sites unrecoverable; a literal can equal an address; compaction of thunk branches depends on absolute addresses, and recompaction would move offsets the side tables recorded; recompiling is a native compilation, which the installation bound excludes. Now: annotate the few dozen sites through helpers listed by the census, guard thunk links and pointer arguments, and leave the unguarded residue to the twin comparison where producer and consumer values differ.
2. Forms and canonical footprints. Sources: draft 1, "Why fixups use the assembler's own writers"; revision 2, "Footprints checked on every import"; the drain's paragraph on strict. First: three forms patched by the native writers, with footprints checked only by reading back after patching, under strict. Evidence: the writers trust their site (an ARM64 `linkJump` that meets a `nop` rewrites the instruction before it, before the allocation at site 0; the x86_64 writers store without reading the opcode), so a section that passed every other check could make a writer write outside its footprint; a separate x86_64 conditional-jump form would add a kind without information, since the opcode decides the length. Now: capture writes the canonical encoding and V3 checks it before any writer runs; once THREAD made strict off by default, normal mode trusts the checksummed bytes (I8).
3. Code symbols and the thunk enumeration. Source: draft 1. Evidence: the consumer would call a thunk generator during import, so a key read from a file must never name code to run, while C++ operations and slow-path functions are only data, and the Linux build has no `JITOperationList` to look them up. Now: `CodeSymbol` offsets from `codeSymbolAnchor` for C++ functions; the closed `BaselineThunk` enumeration, mapped by `JIT::baselineThunkGenerator`, for generators.
4. Side tables and section headers. Sources: draft 1, "Why side tables are offsets and not fixups"; kept minors 2.5, 4.6 and 4.7. First: code addresses of the side tables as fixups; headers with tag, layout, tier and architecture; the code map as pairs of index and offset; a U check for `m_ctiDefault`. Evidence: THREAD carries code addresses as offsets; the directory and the build ID already give type, tier, architecture and layout; the code map holds exactly the instruction starts (N27); nothing reads `m_ctiDefault` at run time (`CodeBlock::baselineSwitchJumpTable` has no caller, and a dense switch reaches its default by a direct jump). Now: offsets, counts-only headers, one offset per instruction start paired at install (U5), and `m_ctiDefault` written for every table without a U check.
5. String switch ranks. Sources: draft 1, "String switch ranks"; the drain's settlement and its rank-case blocker. First: internal branches carry no fixup, as THREAD Capture states. Evidence: which case holds a rank depends on the order of atom addresses in the process; the consumer resolved rank cases before its string tables held code addresses. Now: each rank's case jump carries a fixup, THREAD Capture's one exception, and the consumer fills the tables (10.3, step 7) before resolving (step 8).
6. ARM64 veneers. Sources: draft 1, "Why veneers at the end of the allocation"; revision 1, "A refused charge and ARM64 veneers". First: an inline `b.!cond` over a `b`; later, stopping all recording on a refused charge, or reserving deferral storage in the first charge. Evidence: the inline form turns every not-taken exception check into a taken branch; a refusal then left a choice between an unlinked branch and uncharged memory; the number of deferred jumps is unknown before emission. Now: one veneer per target after the code, groups charged as they grow, and a native link once recording stops or a group's charge is refused (I19).
7. Cached temp registers. Source: draft 1. First: reuse a cached temp for the same reference, as THREAD allows, by tagging `CachedTempRegister`. Evidence: reuse windows in baseline code are short, and the tag adds state to every ARM64 cache path. Now: invalidate after each reference (R4); bench obligation B3 measures the cost.
8. When a compilation records. Sources: the drain's blocker "recording after cache activity is off"; kept minor 9.b; walkthrough "pointers 1"; revision 2, "Super-sampler opcodes". First: record whenever the VM has a producing role, with per-compilation reasons for Debugger, ShadowChicken and profiler opcodes and PC maps; record every compilation of a producing VM, whether or not its UCB has a record; a `ProcessAddress` kind for `g_superSamplerCount`, or the `UnannotatedReference` reason. Evidence: a debugger attach turns cache activity off for good, so no recording compilation meets Debugger-mode bytecode or PC maps (N15, N29); a recording fault ends production while activity stays on; a compilation whose UCB has no record was charged for a record no capture reads, and the registry lookup that tells them apart belongs on the VM thread, where both plan constructors run; only builtin-mode parsing or the fixed-off `exposePrivateIdentifiers` produce the super-sampler opcodes, and `UnannotatedReference` names paths baseline code never takes. Now: `producerContext` follows active production; the plan's `jitCacheRecordsImage` decides; `NotShareable` is the one shareability reason; `SuperSamplerOpcode` marks those compilations.
9. The linked size. Source: revision 2, "The linked size". First: the image's size from the code reference. Evidence: the handle can exceed what `LinkBuffer` wrote (ARM64's `shrink` rounds to the granule; the libpas JIT heap's size classes on both architectures), and the tail keeps stale pool bytes. Now: images and snippets are their linked size for capture, copy and flush, and `commit` samples the handle's size as `JIT::finalizeOnMainThread` does.
10. Charging. Sources: kept minors 7.7 and 7.8. First: charge each allocation; keep a record's step-rounded charge for its life; a compact record that interns targets. Evidence: per-fixup charges sit on the emission path; slack kept for life grows with the number of live bodies; no measurement asks for a compact record yet. Now: steps of `kRecordChargeStep`, slack released when a compilation or regeneration finishes, a rebuilt record charged its exact bytes, and bench obligation B2 naming interning as the first change to measure.
11. MathICs without inline code. Source: revision 1. First: every MathIC has four locations and a slow call. Evidence: when `generateInline` returns false the IC has neither (N17), so a body with string concatenation failed S4 at capture or V5 at import. Now: a state without inline code, read from the IC's null locations after the link tasks, because those locations are the native fact the import reproduces.
12. The inline-start rewrite. Sources: kept minors 3.1 to 3.7. First: record the rewrite in a recording scope of its own and drop every fixup the rewritten bytes touch. Evidence: the rewrite's assembler emits one jump and never draws; a `CannotCompile` body keeps profile-write fixups in the inline region, live before the rewrite and dead after it; a partly covered footprint would leave producer bits outside every fixup. Now: the rewrite stays native, `didRewriteInlineStart` inserts the `SnippetEntry` fixup at V5's site, only wholly covered footprints go, and a partial overlap is `InconsistentRecord`.
13. The MathIC allocation fault. Sources: draft 1, "MathIC regeneration and the allocation fault"; the drain's settlement. Evidence: `generateOutOfLine` repoints the slow call before it allocates the full snippet, unconditionally, so the repoint is no effect of the failure, whose own effect is the native fallback. Now: the fault at each `didFailToAllocate` branch, inside the operation, before the fallback; THREAD Execution now gives the lane MathIC regeneration in every tier.
14. The `negate` operations. Source: draft 1. First: change `operationArithNegateProfiled` to take the IC. Evidence: its other callers pass a profile; `add`, `sub` and `mul` repoint to operations that take the IC; DFG and FTL use `operationArithNegateOptimize`. Now: a new `operationArithNegateProfiledNoOptimize` sharing one static helper, so only baseline code changes.
15. Baked facts. Source: draft 1, "Baked facts". First: compare the capability level exactly through `capabilityLevel()`, and accept a taint mismatch in the direction native sharing tolerates. Evidence: the code depends on the class only through `CannotCompile` (N13); `capabilityLevel()` memoizes into the newborn CB; a taint mismatch leaves the CB native, the safe outcome. Now: the `CannotCompile` predicate through `computeCapabilityLevel` when the memo is unset, and taint compared exactly.
16. Capture. Sources: draft 1, "Capture streams from live memory"; kept minors 2.8, 2.9, 5.1 to 5.3, 5.6 and 5.7; the drain's `ChargeRefused` finding. First: a 64 KiB capture buffer charged to the budget with pre-encoded tables; a second fault raised by the glue for a refused charge; island chains bounded at four hops. Evidence: nothing capture reads changes before JS resumes, and the writer already buffers; the budget raises its own fault; four hops hold only for the default pool, while `jitMemoryReservationSize` sets its size freely and each hop of `islandForJumpLocation` moves more than half the jump range. Now: the writes stream live code with canonical footprints from a stack array, a refusal returns `ChargeRefused` with no second fault, and S1 bounds the chain by the pool's size over half `nearJumpRange`.
17. Validation. Sources: kept minors 4.4, 4.5, 5.4 and 5.5; the drain's paragraph on strict. First: `prepareImage` validates its sections itself, and capture checks every side-table pointer in both modes. Evidence: two owners for one validation; THREAD leaves assumption checks to strict; a validated-view type would buy nothing, since normal mode validates nothing and every caller would take the view unvalidated anyway. Now: the glue validates, under strict, before `compareBakedFacts`, which relies on U4; normal mode trusts what this lane's capture wrote (I2, I3, I8), and debug builds assert it.
18. Preparation and the rebuilt record. Sources: revision 2, "The prepared code before commit"; draft 1, "A rebuilt record the budget refuses"; kept minors 4.a and 6.a. First: the constructed code reachable only through `commit`; a `RecordPolicy` flag beside the budget; a separate fault raised by the glue when the rebuilt record is refused. Evidence: the ICs lane checks its mold pairing against the code before anything is written; the budget pointer already says whether to rebuild; no capture reads a record once production has ended; the budget raises its own fault. Now: `PreparedImage::code()` before `commit`, a rebuild exactly when a budget is passed, and an import that stays valid without a record when the rebuild is refused.
19. Exported headers. Source: revision 4, "Exported headers". First: construct `MathICRegeneration` and call `ImageEmission.h` helpers inside `JITMathIC.h` and `AssemblyHelpers.h`, or export the lane's headers. Evidence: Bun compiles against JSC's copied headers only (14.4); `ImageReference` derives from `CCallHelpers::ConstantMaterializer`, and `CCallHelpers.h` includes `AssemblyHelpers.h`; exporting would hand the lane's internals to Bun; a twins-only data member gives an exported class two layouts. Now: forward declarations, opaque enumerations, pointer members and hooks with JSC types only; `generateOutOfLine` moved to `JITMathIC.cpp` with explicit instantiations; twins-only additions to exported classes are member functions.
20. The scope thunk key. Sources: kept minors 3.8 and 3.9. First: key `get_from_scope`'s thunk call by the profiled resolve type. Evidence: the default branch of `JIT::emit_op_get_from_scope` mixes `if` and `else if`, so `ClosureVarWithVarInjectionChecks` and `GlobalPropertyWithVarInjectionChecks` link `GetFromScopeGlobalVar`; keying by type would change native bytes (I4) and bake a shape no fact records. Now: the key the native chain links, with T20.
21. Check names. Source: walkthrough "undeclared 2". Now: `ImageCheck`'s enumerators take the checks' identifiers, where `CBCheck` uses descriptive names, because this SPEC, its failure table and its tests name every check by identifier, so `status` reports steps such as `image.v3`.
22. The twin CB and its inputs. Sources: draft 1, "Twins"; revision 1, "Creating the twin CB"; revision 2, "The twins producer side" and "The ConsumerProducer's twin data"; revision 4, "Atom-ness at strict-equality sites"; kept minors 8.5 and 8.7. First: create the twin through `newCodeBlockFor` or `newReplacementCodeBlockFor`, or hook between setup and `installCode`; a test hook that writes the twin's capability memo; record only the unguarded scope kinds, each at its emitter's read; compute the atom choice in `snapshotCompileInputs`; later, a twin UCB, or a twins-only input source at every emitter's metadata read. Evidence: `newCodeBlockFor` asserts an empty slot, which `installCode` has just filled, and `CopyParsedBlock` shares the installed CB's metadata (N18); a hook before `installCode` interleaves the twin's link, compile and exceptions with the install; the twin shares the installed CB's executable and source provider, so its level and taint equal what the baked facts compared; emission also reads global resolve types and LLInt modes, which a fresh link can resolve differently, and one snapshot call replaces edits at ten emitter reads; atom-ness only grows, so it cannot be written into a twin, and computing it would copy the templates' selection order; a twin UCB loses the absolute comparison of UCB targets; an input source needs a relaxed `RELEASE_ASSERT` and an edit at every read; a fresh consumer must replay both processes' regenerations. Now: the twin comes from its class's `create()` after `installCode` and its level and taint are checked, never written; inputs 1 to 7 are snapshotted at compile start and written into its own metadata in states the GC reads safely, and the atom choice is recorded at the template's read; a ConsumerProducer's rebuilt record takes its twin data from the imported section, and each capture computes its own producer values.
23. When the twin check runs. Source: revision 5. First: rely on twins runs turning `useConcurrentJIT` off, or a per-lane twin switch in the runner. Evidence: the snapshot equals what emission read only when no JS ran during the compile; the consumer's overwrite of the UCB's arithmetic profiles races compiler threads; other runs set options per run and the option defaults on; a check that runs unsound reports differences that are not defects, and a switch a run can forget leaves it unsound; recording every input at its read would fix only the producer side. Now: the check tests both preconditions and reports a skip, and THREAD Verification's skip rule decides when a skip fails a run.
24. The twin's lifetime. Sources: revision 1 on `didOptimize`; kept minors 8.1 to 8.4, 8.6 and 8.8; "Native-fidelity review after the minors". First: keep every twin CB in a `Strong` list until VM destruction, so its destructor never writes `didOptimize`. Evidence: the list pinned UCBs, executables and realms and changed what dies in twins runs (the UCB lane's `drop-and-reimport.js` failed); for a CB that never received JIT code that write is the destructor's only effect on shared state (N18); `~CodeBlock` can run at process exit while another thread finalizes; a failed twin compile left the profiles overwritten. Now: the twin dies after its check, a process-wide registry that `~CodeBlock` consults first skips the write (a `NeverDestroyed` set its first user creates, since `LazyNeverDestroyed` would need a first caller to construct it, and a `Lock` with no destructor), and a scope object restores the profiles on every exit.
25. Comparing an image with its twin. Source: revision 1, "PC-relative fixups in the twin comparison". First: compare bytes, excluding only the footprints of artifact targets. Evidence: the restored image and its twin are different allocations, so every `Call` and `Jump` fixup holds another displacement, and ARM64 may route through other islands. Now: canonical bytes, fixup lists by logical target, and each side's footprints decoded in its own context; support, VM, UCB, process and structure-base targets thereby compare as absolute values.
26. Relocation domains. Sources: revision 3, "The engine image does not move"; the drain's settlement and its paragraph on producer values. First: group targets by kind and exempt far calls; make twins builds position-independent; later, compare one base per domain per pair of processes. Evidence: Bun's flags build the engine position-dependent (N23), so every `Operation` target and every static atom, such as the `""` constant's `StringImpl::s_emptyAtomString` (N24), resolves alike in every process; THREAD keeps Bun's flags and skips targets inside an object loaded at its link-time address; under those flags every process has the same engine bias, and the heap has no single base (cells in blocks, the VM, atoms and profile vectors in malloc). Now: a target's domain is where it resolves; the clause skips zero-load-bias objects, harmlessly, since such an object never moves within a build; the twins section keeps one producer value per fixup.
27. Moving the domains. Sources: revision 3, "Structure reservation and executable pool", "The heap" and "T3"; revision 7. First: rely on address randomization for every domain and relaunch until bases differ. Evidence: the structure reservation is aligned to its 4 GiB size, so 28-bit randomization on x86_64 shares a base about once in 256 pairs and an 18-bit arm64 kernel nearly always; pools at different bases can overlap, and two processes generate thunks in different orders; heap coincidences are chance; ASan's allocator, which serves the twins build's heap, was measured to move on x86_64, and ARM64 was not measured; T3 described only what the clause detects. Now: the runner places the pool and the structure reservation apart, checks that the heap moves, repeats a run whose only reports are heap coincidences, and T3's hooks force a report in each domain.
28. Bodies a process captured itself. Sources: revision 9; the drain's settlement. First: compare relocation pairs for every import; alternatives: skip the whole check for such imports, or keep a ConsumerProducer from importing its own commits in twins builds. Evidence: a ConsumerProducer imports its own commits after their UCBs die or through a live attach, and a Bun worker imports its main VM's; the domains that coincide are process-wide; skipping everything loses comparisons that need no moved address, and forbidding self-imports makes twins builds unlike production. Now: a 16-byte capture-process token; no relocation pair for a body that carries the importing process's token (now THREAD Verification's own skip); every other comparison still runs.

### 4.3 What leaves the history

These are logs and routine fixes, which SKILL.md keeps out of both files; git keeps them.

- Every round heading and its summary ("Round N filed..."), the status line of draft 1, and "Alignment with the lanes written in parallel" (M8 states its rule).
- Revision 4's closing paragraph (a finding against other lanes, recorded in SPEC-cb-history.md and SPEC-ucb-history.md, which keep their records), revision 6 (THREAD Execution and SPEC-integrator.md section 10 now assign the plan sites) and revision 8 (no change).
- From the drain: its first paragraph, its account of the other THREAD changes beyond what records 2, 8 and 17 keep, and its findings beyond what records 5, 8, 16 and 26 keep.
- Kept minors not named in a record: mismatches 1 and 2; batches 1.1 to 1.10, 2.1 to 2.4, 2.6, 2.7, 4.b, 4.1 to 4.3, 4.8 to 4.10, 5.8 to 5.10, 6.b, 6.1 to 6.8, 7.1 to 7.6, 8.9, 9.a, 9.1 to 9.10, 10.1 and 10.2. Each fixed a pointer, a signature, a task order or a wording the SPEC now states.
- "Compaction (2026-10-07)", whose map this plan replaces; the walkthrough items other than "pointers 1" and "undeclared 2", and its two closing paragraphs; "Options table"; "Runner directives".

### 4.4 Where the SPEC links the history

The header links the file. Beside the decisions: 3.2 (record 2), 3.4 and 3.5 (3), 3.7 (5), 3.9 at R4 (7), 4.1 (8), 4.3 and 4.4 (1), 4.5 (6), 4.7 at step 2 (9), 4.8 (10), 6.1 (11), 6.2 at the move (19), at edit 1 (12) and at edit 5 (13), 6.4 (14), 7 (15), 8.2 (4), 8.5 (17, and 21 at `ImageCheck`), 9 (16), 10.1 and 10.3 step 14 (18), 10.3 step 1 (17), 11.1 (22), 11.3 (23, and 24 at step 2), 11.4 (25, 26, 28), R-INT-11 (27), 14.4 (19), and census D4 (20).

## 5. Contradictions and gaps, left unfixed

1. R-INT-11 asks for `--jitcache-test-image-hook=<name>` "in the shell and the matching Bun environment variable". The user's decision drops every `BUN_JITCACHE_*` variable, test-only ones included; THREAD Session makes Bun's command-line flags its whole configuration; SPEC-integrator.md section 14.2 has Bun take every test flag of harness section 5.1, this one included, and sections 3.3 and 4.5 wire it through `setImageTestHookNamed`. The hosts' mechanics are the integrator's (harness section 5.1, SPEC-integrator.md section 14.2), so R-INT-11 keeps the requirement and cites them, and the stale clause is not carried over. SPEC-integrator.md's plan reports the same clause (its G6). The reviewer and the human should confirm this reading; restoring the clause verbatim is the alternative.
2. Section 17.1's JS-test form `(function main(role, scratch, artifact) { ... })(...arguments)` omits the fourth argument, `sequence`, which SPEC-integrator.harness.md section 7.3 passes. The plan cuts the restatement in favour of the harness, so the SPEC no longer states the stale form.
3. Section 17.1 says every test runs "in `debug-local` (...) and `release-local`". HARNESS.md ("Tests") and SPEC-integrator.md section 16 run tests in the twins build and the plain `debug-local` build and keep `release-local` for benches, and HARNESS.md's cadence never schedules release test runs. The plan keeps the sentence as it is.

## 6. Maps

The data returned with this plan holds every entry below: each old section number of SPEC-image.md as a bare number, each rule and census ID under its own name with the same name as its new value, R10's merge, and the history's top-level headings. Census rows have no section numbers to map: sections A to H of `SPEC-image.sites.md` keep their letters and every row.

### 6.1 Sections of SPEC-image.md

| old | new | action |
|---|---|---|
| 1 | 1 (gains item 8 from 14) | keep |
| 2 | 2 (opening sentence cut as provenance) | keep |
| 3 | 3 (takes 8.1 to 8.4) | keep |
| 3.1 | 3.1 | keep |
| 3.2 | 3.2 (takes 9.4's field positions) | keep |
| 3.3 | 3.3 (takes N26's `static_assert` sentence) | keep |
| 3.4 | 3.9 | move |
| 3.5 | 3.7 | move |
| 4, 4.1 to 4.8 | 4, 4.1 to 4.8 (4.1 takes 6.2's regeneration condition) | keep |
| 5 | 5 | keep |
| 6, 6.1 to 6.4 | 6, 6.1 to 6.4 (6.1's native trace moves to N17, 6.2's log paragraph to 11.1) | keep |
| 7 | 7 | keep |
| 8 | 3 | merge |
| 8.1 | 3.4 | move |
| 8.2 | 3.5 | move |
| 8.3 | 3.8 | move |
| 8.4 | 3.6 (table 8.4 becomes table 3.6) | move |
| 9 | 8 | move |
| 9.1 | 8.1 (takes 14's CPU-feature sentence) | move |
| 9.2, 9.3 | 8.2, 8.3 | move |
| 9.4 | 8.4 (field positions to 3.2) | move |
| 9.5 | 8.5 (takes `ImageCheck` from 10) | move |
| 9.6 | 11.2 | move |
| 10 | 9 (`ImageCheck` moves to 8.5) | move |
| 11, 11.1, 11.2 | 10, 10.1, 10.2 (10.1 loses the order paragraph, cut) | move |
| 11.3 | 10.3 (steps 1 to 14 keep their numbers) | move |
| 12, 12.1, 12.2 | 12, 12.1, 12.2 | keep |
| 13 | 13 | keep |
| 14 | 1 (item 8), 8.1, R-INT-9; its sentence on `start`'s check cut | merge |
| 15, 15.1 to 15.4 | 14, 14.1 to 14.4 | move |
| 16 | 15 | move |
| 17 | 16 | move |
| 17.1 | 16.1 | move |
| 17.2 | 11: data to 11.1, the check to 11.3 (steps 1 to 7 keep their numbers), the comparison to 11.4 | move |
| 17.3 | 16.2 | move |
| 18 | 17 (tasks 1 to 13 keep their numbers) | move |

### 6.2 SPEC-image.sites.md

The preamble and sections A to H keep their places and every row, A7a, A11a and A15a included.

### 6.3 Rule IDs

Every ID keeps its name and stays in its section, so its new place is that section's new number. The data lists each ID with itself as its new value.

| IDs | section, old to new | action |
|---|---|---|
| N1 to N29 | 2 to 2 (N1's last sentence to R3; N15 takes N29's PC-map sentence; N17 takes 6.1's native trace; N26's `static_assert` sentence to 3.3) | keep |
| R1 to R9 | 3.4 to 3.9 | keep |
| R10 | 3.4 to I1 in 15 | merge |
| rules C1 to C5 | 4.6 to 4.6 | keep |
| the unrecordable reasons | 4.1 to 4.1 | keep |
| V1 to V7, U1 to U7 | 9.5 to 8.5 | keep |
| W1 to W4 | 9.6 to 11.2 | keep |
| S1 to S4 | 10 to 9 | keep |
| S5 | 11.3, step 10, to 10.3, step 10 | keep |
| R-UCB-1 to R-UCB-4, R-ALL-1, R-ICS-1, R-INT-1 to R-INT-12 | 12.2 to 12.2 | keep |
| M1 to M8 | 15.3 to 14.3 | keep |
| I1 to I22 | 16 to 15 (I1 absorbs R10) | keep |
| T1 to T21 | 17.1 to 16.1 | keep |
| bench obligations B1 to B5 | 17.3 to 16.2 | keep |
| tasks 1 to 13 | 18 to 17 | keep |
| census rows A1 to A18, A7a, A11a, A15a, B1 to B9, C1 to C27, D1 to D10, E1 to E7, F1 to F5 | `SPEC-image.sites.md`, same sections | keep |

### 6.4 History

| old | new record (section 4.2) | action |
|---|---|---|
| Draft 1: heading and status line | none | cut (log) |
| Draft 1: assembler's own writers | 2 | merge |
| Draft 1: explicit helpers; alternatives rejected | 1 | merge |
| Draft 1: code symbols and an enum | 3 | merge |
| Draft 1: veneers at the end | 6 | merge |
| Draft 1: cached temp | 7 | merge |
| Draft 1: side tables as offsets | 4 | merge |
| Draft 1: string switch ranks | 5 | merge |
| Draft 1: MathIC allocation fault | 13 | merge |
| Draft 1: the `negate` fix | 14 | merge |
| Draft 1: baked facts | 15 | merge |
| Draft 1: capture streams | 16 | merge |
| Draft 1: rebuilt record refused | 18 | merge |
| Draft 1: twins | 22 | merge |
| Draft 1: alignment with the lanes | none | cut (log; M8 holds the rule) |
| Revisions 1 to 9: headings and round summaries | none | cut (log) |
| Revision 1: MathICs without inline code | 11 | merge |
| Revision 1: creating the twin CB | 22, 24 | merge |
| Revision 1: PC-relative fixups | 25 | merge |
| Revision 1: refused charge and veneers | 6 | merge |
| Revision 2: linked size | 9 | merge |
| Revision 2: footprints checked | 2 | merge |
| Revision 2: super-sampler opcodes | 8 | merge |
| Revision 2: prepared code before commit | 18 | merge |
| Revision 2: twins producer side | 22; one clause to SPEC 11.3 step 3 | merge |
| Revision 2: ConsumerProducer's twin data | 22 | merge |
| Revision 3: engine image | 26 | merge |
| Revision 3: structure reservation and pool; T3 | 27 | merge |
| Revision 3: the heap | 27; its reason to SPEC R-INT-11 | merge |
| Revision 4: atom-ness | 22 | merge |
| Revision 4: exported headers | 19 | merge |
| Revision 4: closing paragraph | none | cut (log; the CB and UCB histories hold the finding) |
| Revision 5 | 23 | merge |
| Revision 6 | none | cut (log; THREAD Execution, SPEC-integrator.md 10) |
| Revision 7 | 27 | merge |
| Revision 8 | none | cut (log) |
| Revision 9 | 28 | merge |
| Drain: settlements of the provisional choices | 5, 13, 26, 28 | merge |
| Drain: strict, debugger and activity changes | 2, 8, 17 | merge |
| Drain: findings kept | 5, 8, 16, 26 | merge |
| Drain: the rest | none | cut (log) |
| Kept minors 2.5, 4.6, 4.7 | 4 | merge |
| Kept minors 2.8, 2.9, 5.1 to 5.3, 5.6, 5.7 | 16 | merge |
| Kept minors 3.1 to 3.7 | 12 | merge |
| Kept minors 3.8, 3.9 | 20 | merge |
| Kept minors 4.4, 4.5, 5.4, 5.5 | 17 | merge |
| Kept minors 4.a, 6.a | 18 | merge |
| Kept minors 7.7, 7.8 | 10 | merge |
| Kept minors 8.1 to 8.4, 8.6, 8.8 | 24 | merge |
| Kept minors 8.5, 8.7 | 22 | merge |
| Kept minor 9.b | 8 | merge |
| Kept minors, every other item | none | cut (routine fixes the SPEC states) |
| Native-fidelity review | 24 | merge |
| Compaction (2026-10-07) | none | cut (log; replaced by this plan's maps) |
| Walkthrough: pointers 1 | 8 | merge |
| Walkthrough: undeclared 2 | 21 | merge |
| Walkthrough: the other items and closing paragraphs | none | cut (routine) |
| Options table; Runner directives | none | cut (log) |

### 6.5 Citations in other files that the renumbering moves

Rule IDs keep their names, so only section citations and the two history citations below change. Every citation of 4.1, 4.7, 6.2, 6.4 and 12.1 stays valid.

| file and place | cites | becomes |
|---|---|---|
| SPEC-integrator.md, sections 4.5 and 15.1 (M2) | 15.4 | 14.4 |
| SPEC-integrator.md, section 4.5 (`setImageTestHookNamed`) | 17.2 | 11.3 |
| SPEC-integrator.md, section 6.4 (`codeSymbolAnchor`) | 8.2 | 3.5 |
| SPEC-integrator.md, sections 7.3 (twice), 9.1 and 9.7 | 10 | 9 |
| SPEC-integrator.md, section 8.2 (step 3) | 9.5 | 8.5 |
| SPEC-integrator.md, section 8.2 (steps 10 and 12) and section 9.1 | 11.3 | 10.3 |
| SPEC-integrator.md, section 16.2 (`exec-alloc-faults.js`) | 17.2 | 11.3 |
| SPEC-integrator.container.md, section 8.2 (the sink of a streamed source) | 10 | 9 |
| SPEC-integrator.harness.md, sections 3.1, 3.3 (three times), 7.4 and 11.5 | 17.2 | 11.3 |
| SPEC-integrator.harness.md, sections 4 and 7.5 | 17.2 | 11.4 |
| SPEC-integrator.harness.md, section 8.3 | 15.2 | 14.2 |
| SPEC-ucb.md, section 13.3 (`atom-constants.js`) and the paragraph on writes to published UCBs | 17.2 | 11.3 |
| SPEC-ucb-history.md, kept minor 5.1 (profiler options) | 14 | 1 (item 8), which points to options.md |
| SPEC-ucb-history.md (three citations), SPEC-ics-history.md, SPEC-cb-history.md (three citations) | 17.2 | 11.3 (step 2 where they name it) |
| SPEC-integrator-history.md (two citations) | 15.4 | 14.4 |
| SPEC-integrator-history.md (twin CB, step 2; the capture-process token) | 17.2 | 11.3, step 2; 11.4 |
| SPEC-integrator-history.md, its citations of SPEC-image-history.md revisions 3 and 7 | history | record "Moving the domains" |
| options.md, free row `useConcurrentJIT` | 17.2 | 11.3 |
| HARNESS.md, "Tests" | 17.2 | 11.3 |
| HARNESS.md, "Each lane's twins" | 17.2 | 11 |

options.md's row for `jitMemoryReservationAddress` cites R-INT-11 for the two placement methods, which R-INT-11 keeps, so it needs no change.

## 7. Cross-set cuts, and the borders with the other plans

Each cut below removes a restatement whose rule another file holds in its current text, and whose owner's plan keeps it. The `Off`-role sentence of 17.1 is the one exception: R-ALL-4 holds it once the integrator plan places the ucb plan's move X1 there, and that plan's table of what each lane keeps of the rule leaves this lane nothing (plan-integrator.md section 8).

| what this set cuts | owner |
|---|---|
| 11.1's paragraph listing the install glue's order | SPEC-integrator.md section 8.2, steps 3 to 16, and II12 |
| 11.3's closing sentence on what native setup publishes, retargets, builds and parks | SPEC-integrator.md section 8.2, step 13 (N12 of this set holds what setup reads) |
| R-INT-1: what ends production and turns activity off; a debugger attached before `start` | SPEC-integrator.md sections 5.2, 5.3 and 4.2 (step 5) |
| R-INT-4: the restated contract of `didFailExecutableAllocation` | SPEC-integrator.md section 5.4 |
| R-INT-11: how the integrator creates `Twins` at the VM's first stash | SPEC-integrator.harness.md section 3.3 |
| R-INT-11: the image-hook flag's host mechanics, with the stale Bun environment variable | SPEC-integrator.harness.md section 5.1; SPEC-integrator.md sections 4.5 and 14.2 |
| R-INT-11: a run that keeps `useConcurrentJIT` on skips every import without failing | SPEC-integrator.harness.md section 7.4, under THREAD Verification's skip rule |
| 17.2's preconditions paragraph: what a run that keeps concurrency on does with its skips; `useConcurrentJIT` being free | SPEC-integrator.harness.md section 7.4; options.md, free row `useConcurrentJIT` |
| R-UCB-1: the UCB lane's strict coverage and the atom contract's details | SPEC-ucb.md section 10.1 |
| Section 13: the integrator raising the baseline plan's fault | SPEC-integrator.md section 10 |
| Section 14: `start` checks effective values after `Config::finalize` | SPEC-integrator.md sections 6.3 and 4.2 (step 4) |
| 15.2: the unified-sources naming rule | SPEC-integrator.md R-ALL-8 |
| 15.4: which `jitcache/` headers Bun includes | SPEC-integrator.md section 4.5 and M2 |
| I21: what an exported header may reach | SPEC-integrator.md II18 |
| 17.1: the JS-test conventions (the oracle's comparison, the body function, role-independent output and heap, `jitcache-heap: off`) | SPEC-integrator.md R-ALL-4; SPEC-integrator.harness.md sections 7.2, 7.3 and 7.6 |
| 17.1: the `Off`-role sentence (the same program without `delta` and without assertions about imported or captured state) | SPEC-integrator.md R-ALL-4, as the integrator plan rewrites it with the ucb plan's move X1 (plan-integrator.md section 2, new 12, and section 8) |
| Census C20 note: why `exposePrivateIdentifiers` stays off | options.md, fixed row `exposePrivateIdentifiers` |

This set moves no requirement into another set.

The other plans cut restatements in favour of this set, and this plan keeps each of them: SPEC-integrator.md's plan relies on R-INT-12 and section 4.1 for `jitCacheRecordsImage`'s semantics, on section 10 and section 4.1 for when a record is `Complete`, on M1 for this lane's `Sources.txt` files, on 17.2 step 2, the `Twins` declaration and R-INT-11's first item for the twin-check state's lifetime, and on 17.2's preconditions and R-INT-11's runner item for when the image check skips; SPEC-ucb.md's plan relies on R-UCB-1's list of index spaces; SPEC-ics.md's plan relies on 11.1 and 12.1 for `PreparedImage::code()`, on R-ICS-1, and on census section E; SPEC-cb.md's plan relies on 17.2 for the image check's skip. For the same reason this plan keeps R-INT-2, R-INT-12 and R-ICS-1 whole, and keeps in R-INT-11 the requirements that SPEC-integrator.harness.md sections 1, 3.3, 4, 5.1, 7.3, 7.4, 7.5 and 8.3 cite.
