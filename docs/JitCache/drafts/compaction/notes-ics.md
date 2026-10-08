# Notes: ics set rewrite

Working notes for the rewrite of SPEC-ics.md and SPEC-ics-history.md from plan-ics.md. Not binding.

## Progress

- [x] Read SKILL.md, the required references, THREAD.md, the plan, both files of the set.
- [x] Checked the cross-set targets the plan relies on (current numbers): SPEC-integrator.md 5.4 (site table names `exec-alloc.ic-handler`), 8.2 step 9, 8.3 `Abandoned`, 8.4, 9.1, 9.2, 9.4 steps 4, 6, 7, 9.5 to 9.7, R-ALL-4, R-ALL-8; harness 5.2, 7.2, 7.3, 7.4, 7.6 (names T6), 8.1, 8.2 (quotes R-INT-9's phrase); SPEC-image.md R-ALL-1, R-ICS-1, 11.1, 12.1; SPEC-image.sites.md section E; SPEC-cb.md I16, 3.3, 4.3, 6.4, R-INT-5; SPEC-ucb.md 10.1.
- [x] SPEC-ics.md written (111640 to about 106.6k bytes).
- [x] SPEC-ics-history.md written (50835 to 23743 bytes), 19 entries in the SPEC's order.
- [x] Verification: every rule ID and sub-item defined once, as before; code blocks and the E3 table identical apart from the block order in section 6 and one comment citation; every in-set section citation resolves to the new numbering; every history link resolves to a heading and every entry is linked; no em or en dash; file paths left only in E headings, section 13, `functionNoDFG` in jsc.cpp, `isMegamorphic` in InlineCacheCompiler.cpp, Repatch.cpp for E3's placement and the offlineasm files.
- [x] Sentence-level diff of old against new SPEC reviewed for dropped requirements; three restored (E1's `loadConstantOrVariableCell`, L4's "none of these runs during a capture", R-CB-2's full list of what SPEC-cb.md meets it with).

## Decisions taken while rewriting

- Citations into other sets keep their current numbers; the references pass retargets them.
- The comment in 6.2's code block that cited prepare as "section 6.1" now cites "section 6.2": a citation update through the map, the only byte changed in a code block.
- History anchors are GitHub heading slugs of the entry titles.
- Rows of 3.1 and 3.2 whose value is the derivation's and whose THREAD source is the same are merged into one row each.
- 4.5 states normal mode for the whole lane, including "no call into the lane can fail" from old 10.1's closing sentence.
- History links: placed at every place the plan names except where a nearer link already covers the same decision (T3 to T5 point to the anchor paragraph; T12, T13 and task 7 to R-INT-7, R-INT-9, 11.1, T8 and task 6; 6.2 to 4.5 for the checks entry).

## Revision after the first review

One major finding: R-IMG-1 had been compressed into one sentence that had to be read twice ("gives prepare, readable before `commit` ..., the `BaselineJITCode` setup will install"). Fixed in 8.3; no map change, R-IMG-1 keeps its ID and place.

- Checked the reviewer's claim that nothing was lost, against SPEC-image.md in both the base and the working copy. Its preparation interface (base 11.1, working 10.1) states `code()` valid until `commit` or destruction, `commit` returning `code()`, and the molds in mold order with the producer's fields, and its mold encoding (working 8.2) carries `canBeMegamorphic` as flag bit 4. The base's "setup builds IC i from mold i natively" is in this SPEC's 4.2 and C1. "Unchanged from prepare to setup" follows from the identity R-IMG-1 states, from I1 (prepare writes nothing to the prepared code) and from image 10.1 (other lanes' preparations write nothing to it).
- Took the reviewer's suggested shape with three changes. I kept the full signature, which the base and the first rewrite both name. I restored "that `commit` will return" from the base's "the same object `commit` returns": it is the Image lane's half of the identity that 6.2 cites R-IMG-1 for. I wrote "the accessor's contract" because "its" could refer to the code or its field. I also added the history link to "Prepare reads the prepared `BaselineJITCode`", which the plan places at R-IMG-1 and the first rewrite left only at 6.2.
- 6.2 had the same reduced-relative construction ("the molds prepare checks are the ones ..."); one inserted "that" fixes it. Nothing else changed.
- The cross-set citation stays at the base numbers ("SPEC-image.md sections 11.1 and 12.1"), as the rewrite prompt requires. The image map takes 11.1 to 10.1 and keeps 12.1, and the references pass retargets it.

## Dependencies to watch

- 11.2's `Off` bullet cites SPEC-integrator.md R-ALL-4 for the `Off` rule. At the time of this rewrite R-ALL-4 does not yet hold it; the integrator plan (its section 8 and new section 12) adds the sentence from the ucb plan's X1. If the integrator rewrite does not add it, the bullet comes back whole (plan-ics.md section 2, Section 11).
- SPEC-integrator.md 9.4 step 7 cites ICs R-INT-2 for "P is zero for every body the ICs lane reported a polymorphic site for". R-INT-2 keeps the bit it passes to the CB lane; the P itself now lives only in SPEC-cb.md I16 and the integrator's own step.
- SPEC-cb-history.md cites SPEC-ics.md section 6.3 (now 6.1) and SPEC-integrator-history.md cites section 10.2 (now 10); the references pass retargets them through the map.
