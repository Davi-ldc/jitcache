# Notes: rewriting the integrator set

Progress of the rewrite that plan-integrator.md drives. Old numbers are those of `base/`.

## Status

| file | state | bytes (old to new) |
|---|---|---|
| SPEC-integrator.md | done | 177031 to 164182 |
| SPEC-integrator.container.md | done | 50615 to 49322 |
| SPEC-integrator.harness.md | done; it gained N16 to N25 | 84674 to 91474 |
| SPEC-integrator.maintenance.md | done | 15158 to 15197 |
| SPEC-integrator-history.md | done: 40 decision records, each linked from the SPECs | 96958 to 35602 |

## Conventions chosen

- History links: `([history](SPEC-integrator-history.md#<anchor>))`, GitHub anchors of the record titles the plan's section 6 lists. Every SPEC header links the history file. A check that every link resolves to a record heading and every record is linked at least once passed (40 of 40).
- Citations inside the set use the new numbers: SPEC-integrator.md old 3 to 18 per the plan's map; container 10 becomes 9; harness 3.1 to 3.3 become harness 3. Citations into other sets, THREAD, HARNESS.md and options.md keep their snapshot numbers.
- N16 to N25 move to the harness, which cites them bare (N24) as its own and cites the main file's facts as "SPEC-integrator.md N9".

## Old-to-new numbers, SPEC-integrator.md

3 -> 14 (3.1 -> 14.1, 3.2 -> 14.2, 3.3 -> 14.3); 4 -> 3 (4.x -> 3.x); 5 -> 4 (5.1 -> 4.1, 5.2 -> 4.2, 5.3 -> 4.4, 5.4 -> 4.5, 5.5 -> 4.6); 6 -> 5 (6.1, 6.2, 6.3 -> 5.1; 6.4 -> 5.2); 7 -> 6 (7.1 to 7.3 -> 6.1 to 6.3; 7.4 -> 12); 8 -> 7; 9 -> 8; 10 -> 9; 11.1 cut, 11.2 -> 4.3; 12 -> 10; 13 -> 13; 14 -> 11; 15 -> 14 (15.1 -> 14.4, 15.2 -> 14.5); 16 -> 15; 17 -> 16; 18 -> 17.

## Checks run

- Sentence-level comparison of each old file against the new set, with digits normalized: every fragment not found verbatim was read and is either a cut the plan names with its owner, a move, a renumbered citation or a rewording that keeps the requirement.
- Section citations inside the set were grepped and checked against the map; no citation of an old number remains inside the set (the history's old records are gone).

## Deviations from the plan

None changes a section or ID's destination, so the map stands as planned. Content points where the text keeps more than the plan said:

- 15.2, `exec-alloc-faults.js`: the plan drops the reasons the MathIC site needs no further evidence. Its reason carries a test-design requirement, that the function whose MathIC regenerates stays in baseline through the `delta`, so the row keeps that requirement in one sentence and the reason goes to the record "What the executable-allocation fault tests can see". The row also names the reoptimization count at which `adjustedCounterValue` clips, since the sentence that defined "that count" was cut.
- 3.4 keeps "is exposed only in C++ (THREAD Session)", a citation, where the plan left the opening unmentioned.
- 3.2 step 7: the closing I/O sentence names a failed `flock` and a failed listing, so the rows of container 9 and the clause of container 2's `tryAcquire` step 2 that the plan cuts land visibly at their owner.
- Harness 12 takes container 8.2's clause that no reader sees a temporary, since container 8.2 now cites harness 12 for what a killed producer leaves.
- 8.2: a strict rejection of a saved summary "stops the capture with nothing written", the effect old 11.1's row gave, which no step stated.

## Gaps

The plan's G1 to G10 still hold as it states them; the rewrite keeps each passage as written. No new contradiction with THREAD was found.
