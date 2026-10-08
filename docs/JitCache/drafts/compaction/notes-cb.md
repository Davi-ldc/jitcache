# Rewrite notes: the cb set

Working notes for the cb rewrite under plan-cb.md. Sources: base/SPEC-cb.md and base/SPEC-cb-history.md (identical to specs/ at start).

## Progress

- [x] Read SKILL.md, the required references, THREAD.md, options.md, plan-cb.md, both cb files.
- [x] Checked the cross-set owners the plan cuts to: SPEC-integrator.md R-ALL-4 (the `Off`-role sentence is not there yet; cut 8 relies on the integrator rewrite), R-ALL-8, section 5.3 (`tryCharge`, `release`), IB10; SPEC-ucb.md section 8.4; SPEC-ics.md R-CB-1; harness sections 7.2, 7.3 (`--destroy-vm` on the jsc command line), 7.4 (`useConcurrentJIT` default for `cb/`), 7.6; SPEC-image.md section 17.2.
- [x] SPEC-cb.md written (105,023 to 105,013 bytes; the plan adds a design in brief and 23 history links, and keeps nearly every passage). After the review fixes, 105,574 bytes.
- [x] SPEC-cb-history.md written (66,470 to 17,444 bytes): a two-sentence header and 17 entries in SPEC order, as the plan lists them. After the review fixes, 17,681 bytes.
- [x] Review fixes applied (section below); the rule-ID census, the code-block comparison and the dash check rerun clean.
- [x] Rule-ID census: the set of IDs (P, N, A, F, V, S, SC, R-UCB, R-INT, I, E, M, U, B) in the new SPEC equals the base's.
- [x] Code blocks: every fenced block is byte-identical to the base.
- [x] Anchor check: the 17 history headings and the anchors the SPEC links are the same set.
- [x] No em or en dashes in either file.

## Decisions while rewriting

- SC2 and SC3 sit as sub-bullets of capture step 2 and SC1 of step 6, each with its fault; `scoreLive` cites step 2 for both and keeps its normal-mode `ASSERT` of SC2 alone (plan F2).
- S1 to S3 sit in 5.2 after the sentence that `prepare` runs `validateState` first; one sentence names their class (invalid material, section 7).
- 3.4's intro now says `validateState` is the one implementation of V1 to V15 and points to 5.2 and 4.4 for where it runs.
- The "Two sections" link sits in section 3's opening sentence, where the two sections are introduced, instead of inside 3.3, which opens with its code block.
- 5.3: the polymorphic paragraph merged into the opening; its warm-up rationale (the consumer's invocations rebuild the cases they meet) left for the history entry, as the plan's list of kept facts allows.
- 11.2's lead sentence said live tests build their CBs "as SPEC-ics.md's live tests do", but SPEC-ics.md uses the newborn CB only inside its `DeferGCForAWhile`, while U8 runs entry points with no deferral on a CB kept in a local. The lead now cites SPEC-ics.md only for the global object and functions and states that the recipe follows; the recipe itself is unchanged.
- R-INT-6's last sentence now reads that THREAD Verification's skip rule decides whether a skip fails the run, which is what that rule says; the old "which fails the run" overstated it.
- 9.2's Bun sentence: "which no Bun code calls" became "has no caller in Bun"; same fact.
- History entry 1 adds one clause from kept minors 1.7 and 1.8: the explicit THREAD statement also wins for the exit sites and quick tier-up bits the UCB lane carries.

## Review fixes

Two majors from the set review, both accepted after checking the source; the SPEC is now 105,574 bytes and the history 17,681.

- N8's one-to-one mapping. The plan moved to the history the sentence that native bytecode maps `op_new_array_buffer` metadata entries to instructions one to one, reasoning that no rule depends on it. S2 and V8 at SC1 do, since each must accept every native CB. `CodeBlock::finishCreation` placement-constructs each instruction's entry (`INITIALIZE_METADATA`) and `link_arrayAllocationProfile` writes that instruction's `m_recommendedIndexingType` into it, so a shared entry would keep its last writer's type; an entry no instruction names keeps the zeroed bytes of N1, outside V8's F19 types. Checked that the fact still holds: every generated `emit` allocates its own entry through `addMetadataFor`, `BytecodeGenerator::rewind` removes only `OpMov`, `OpTypeof` and fused compare and test ops, and `BytecodeGeneratorification::run` replaces only `op_yield` and the generator-frame instruction. The sentence is back in N8, placed before the "therefore" it supports, and the linking sentence now says each entry gets its own instruction's type. 9.3's last item names the facts behind V8's bound and S2's F19 check, the mapping among them. The history entry cites N8 instead of repeating the evidence and adds a paragraph on why N8 keeps the mapping, so a later pass does not move it again.
- 11.3's concurrency subject. The bare citation made a corpus author open harness section 7.4 to learn whether the second run must declare `jitcache-expect-twin` skips. The sentence now states that the Image check skips every import of that run (SPEC-image.md section 17.2) and that the runner treats those skips as no difference, since the run's options turn `useConcurrentJIT` on (SPEC-integrator.harness.md section 7.4). Citations into other sets keep the old section numbers for the retarget pass.

## Differences from the plan map

None in the map: every section keeps its number, 3.4 is retitled "Validation", S1 to S3 moved to 5.2 and SC1 to SC3 to 4.4, as the map says. Two passages depart from the plan's passage notes after review: N8 keeps the one-to-one sentence the plan's N8 bullet and history entry 6 moved out, and the concurrency subject of 11.3 keeps the outcome that cut 1 replaced with citations.

## Gaps found, left unfixed

- THREAD Session classes a failed strict check as invalid material when it rejects bytes read from the artifact and as a recording fault when it rejects what a capture builds. S1 and S2 reject the consumer's own newborn CB, which neither class names; section 7 makes them invalid material (plan F1).
- In normal mode `capture` `ASSERT`s both SC2 and SC3, while `scoreLive` `ASSERT`s SC2 alone, though both read the UCB copies at the same positions (plan F2). Kept as written.
- Cut 8: 11.3 cites SPEC-integrator.md R-ALL-4 for the `Off`-role convention, which depended on the integrator rewrite adding it. Resolved: R-ALL-4 now states it.
