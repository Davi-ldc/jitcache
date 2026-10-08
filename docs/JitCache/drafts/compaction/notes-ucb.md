# Notes: rewriting the ucb set

Working notes for the compaction of SPEC-ucb.md, SPEC-ucb.codec.md and SPEC-ucb-history.md from plan-ucb.md. Sources are the snapshot in `base/`, identical to `specs/` when this pass started.

## Progress

- [x] SPEC-ucb.md (221,829 to 215,043 bytes after the review fixes)
- [x] SPEC-ucb.codec.md (34,113 to 33,880 bytes)
- [x] SPEC-ucb-history.md (101,532 to about 39,500 bytes, 26 records)
- [x] final pass: internal citations renumbered, all 26 history anchors linked and present, every rule ID still defined, a clause-level comparison of old and new SPEC and codec reviewed line by line

## Section map applied (old -> new, SPEC-ucb.md)

1->1, 2->2, 3->15.1, 4->3 (4.1->3.1, 4.2->3.2, 4.3->3.3 and 3.4, 4.4->3.5, 4.5->3.6, 4.6->3.8), 5->6 (5.1->6.1, 5.2->6.4, 5.3 and 5.5->6.3, 5.4->6.2, 5.6->6.5), 6->7 (6.1->7.1, 6.2.1->7.2.1, 6.2.2->7.2.2, 6.2.3->3.7, 6.2.4->7.2.3, 6.2.5->7.2.4, 6.2.6 and 6.2.7->7.2.5, 6.2.8->7.2.6, 6.3.n->7.3.n, 6.4->7.4, 6.5->7.5, 6.6->7.6, 6.7->7.7), 7.n->4.n, 8.n->5.n, 9.n->8.n, 10.n->9.n, 11.n->10.n, 12->11, 13->13, 14->14, 15->15.2, 16->12, 17->15.3.

Old "section 4.3" splits by subject: what a context holds -> 3.3; holder digests, TDZ chains, environment digests, when digests are computed, what a record keeps -> 3.4.

Old "section 6.3.1, step 4" splits: stored key and core kind -> 4a; provenance -> 4b; context -> 4c; holder (C11) -> 4d; S2 -> 4e.

Codec citations of the main file renumbered: 4.3 -> 3.4, 6.2.5 -> 7.2.4, 6.3.1 -> 7.3.1, 6.3.2 -> 7.3.2, 6.3.3 -> 7.3.3, 7.2 -> 4.2.

## Decisions taken while writing

- F19 takes the native decoder's mechanism (`CachedJSValue::decode`, `decodePlainString` unless the decoder holds the atom) that E10 told; E10 cites F19, and F19 no longer cites E10. Without this the two would cite each other and neither would state the mechanism.
- X1 (the `Off`-role convention): SPEC-integrator.md R-ALL-4 in `specs/` does not hold it yet, so section 13.3 keeps the bullet whole, as the plan's fallback says. The integrator plan moves it into R-ALL-4; once that lands, the cross-set pass can cut ucb's copy to its lane clause ("without statistics").
- Three clauses the plan cut as restatements were not fully held by their named owner, so each moved instead: 13.2's "without the argument the call collects nothing and returns no `registryViolations`" into M4; old 11.1's "a natively decoded one goes on to be published natively without seeds" into 7.3.3 step 3; old 11.2's "S2 runs at the import, never at a decode, an attach or a capture" into 3.5.
- The stack paragraph of 7.3.1 keeps "from deeper in the stack" and "a later request at a shallower depth imports", which the plan's one-sentence reason would have lost.
- History headings are the record titles without the H labels; the SPEC and codec link them by GitHub anchors, as `([history](SPEC-ucb-history.md#anchor))` at the end of the passage. The codec header links the history too.
- Citations into other sets keep their snapshot numbers; the integrator's plan-site section is cited as SPEC-integrator.md section 10, and F24 cites SPEC-integrator.md N9 for the DFG and FTL branches it no longer lists.

## Map differences

- E10's account of the native decoder -> F19 (merge).
- 13.3 convention bullet 3 (`Off` role) -> 13.3, kept whole (X1 not yet in R-ALL-4).
- 13.2, last clause of the `verifyRegistry` paragraph -> 15.2 M4 (move).
- 11.1 closing, natively decoded UCB published without seeds -> 7.3.3 step 3 (move).
- 11.2 S2 paragraph, "never at a decode, an attach or a capture" -> 3.5 (merge).
- 4.5 -> 3.6 and 6.2.3 -> 3.7, where the plan had 3.7 and 3.6: the functions come before the request-key table that calls them, as in the old order. Cross-set retargeting must use these two numbers.

## Review fixes

- Blocker, section 3.5: the merged sentence said a wrong supplied digest could only make an attached generated or imported UCB miss. S1 runs only with strict on and fails as invalid material (7.3.2 step 10, 10.2), and with strict off nothing compares such a UCB's core. The sentence is split again as in old 4.4: a natively decoded UCB misses by its core in both modes; with strict on, S1 makes a difference invalid material; an import acts on the key alone, hence S2.
- Major, section 3.7: the table read functions and request members defined later. Functions now come first (3.6), and the table's lead cites section 7.1 for the `RequestState` members (`requestFeatures` is the snapshot, `requestMode` the request mode) and section 6.1 for a UFE's identity (`RootIdentity`); the direct-eval row names `DirectEvalExecutable::create` (section 7.2.3) where it said `create`. The sentence defining "the UCB's mode" moved above the table, which uses it. Internal citations renumbered in SPEC-ucb.md (3.4, 3.5, 7.3.1 steps 1 and 4c, 7.3.3 step 1, 7.7, U3) and in the history's context-digest record.

## Gaps

The plan's G1 to G9 stand as written. Found while writing:

- X1: SPEC-integrator.md R-ALL-4 in `specs/` now holds the `Off`-role convention, so the second paragraph of SPEC-ucb.md section 13.3 repeats it; only its lane clause ("without statistics") is ucb's own. Left for the cross-set pass, outside the review fixes.
