# Rewrite notes: the image set

Working notes for the rewrite of SPEC-image.md, SPEC-image.sites.md and SPEC-image-history.md from plan-image.md.

## Progress

- [x] SPEC-image.md: written in the plan's outline (sections 1 to 17). Rule-ID check against base: every `- ID.` definition present except R10, which the plan merges into I1. 27 history anchors linked; the 28th (the scope thunk key) goes in the census D4 note.
- [x] SPEC-image.sites.md: preamble (table 3.6, sections 4.3 and 14.4, history line), C6/C7 note (11.1), C20 note (options.md), D4 note (history link), E note (8.2), F note (N17). Every row unchanged.
- [x] SPEC-image-history.md: the 28 records of plan section 4.2, ordered by the SPEC's sections with "Finding references" first; every anchor the SPEC and census link resolves to a heading, and every heading is linked.
- [x] citation sweep inside the set: every "section N" and "table N" in SPEC-image.md and the census resolves under the new numbering; the remaining old-looking numbers (4.2, 5.1, 5.4, 7.1, 7.2, 9.1, 10, 13.3, 14.2) are citations of SPEC-integrator.md, its harness or SPEC-ucb.md, left for the references pass.
- [x] sentence check: the base SPEC split into sentences, and each of the 274 sentences missing verbatim from the new file checked by hand. Each is a retargeted citation, a plan-listed move (N1's last sentence, N26's `static_assert`, 6.1's trace, 6.2's log paragraph, section 14) or a plan-listed cut (sections 2 and 7 of the plan).

## How the plan was applied

- History records follow the plan's titles and content. They run in the SPEC's section order, as plan section 4.2 asks, with "Finding references" first because section 3 and sections 4.3 and 4.4 rest on it; records are cited by title, so the order changes no citation.
- R-INT-1 cites SPEC-integrator.md sections 4.2 (step 5), 5.2 and 5.3 for what ends production, the owners plan section 7 names.
- R-INT-4 cites SPEC-integrator.md section 5.4 for the contract; the section 13 row cites SPEC-integrator.md section 10.
- 16.1's JS-test paragraph keeps only the citation of R-ALL-4 and harness sections 7.2 to 7.6.

## Review fixes

- Blocker, R-INT-11's Bun environment variable: restored (gap 1 below).
- Major, R6's last sentence: the rewrite, following the plan, had turned it into "Section 4.5 says how such a branch is linked once a recorder stops recording (I19)", whose "such a branch" read as the x86_64 `jcc` named just before. R6 now states the rule in one sentence and cites where it lives: once a recorder stops recording, an ARM64 conditional external branch is linked natively, with no veneer (section 4.5, I19), and that recorder's record is never captured (section 4.8). The last clause is the base's, which explains why a branch with no fixup leaves I1 intact.

## Map changes

None: every old section, rule ID and history heading lands where plan section 6 puts it.

## Gaps (reported, not fixed)

1. R-INT-11, hook flag: the clause is back word for word from the base, with only its section number retargeted (17.1 to 16.1), because dropping it would change what the integrator must build, and this pass changes form only. It asks for `--jitcache-test-image-hook=<name>` "in the shell and the matching Bun environment variable". That contradicts THREAD Session ("Bun's command-line flags, which are Bun's whole user-facing configuration"), SPEC-integrator.md section 11.2 (base 14.2) and its `JSCInitialize` row, where Bun's `JSCInitialize` passes the flag `--jitcache-test-image-hook` to `setImageTestHookNamed`, and SPEC-integrator.harness.md section 7.7, which hands Bun its `--jitcache` run options as flags and keeps every JITCache flag off the `BUN_JSC_` route. The human decides whether the clause goes; plan gap 1 and plan-integrator.md G6 report the same clause.
2. Old 17.1 stated the JS-test form `(function main(role, scratch, artifact) { ... })`, without the fourth argument `sequence` that SPEC-integrator.harness.md section 7.3 passes. The restatement is cut in favour of R-ALL-4 and the harness, so the stale form is gone.
3. 16.1 still says every test runs in `debug-local` and `release-local`, while HARNESS.md ("Tests") and SPEC-integrator.md section 15 (base 16) run tests in the twins build and plain `debug-local` and keep `release-local` for benches. Kept as written (plan gap 3).
4. The `Off`-role sentence of old 17.1 is cut on the strength of the integrator plan placing the ucb plan's move X1 in SPEC-integrator.md R-ALL-4 (plan-integrator.md section 2, new 12). Resolved: the rewritten R-ALL-4 now holds it ("Under the `Off` role ... a script runs its Consumer path without `delta` and without any assertion about the state JITCache imported, seeded, attached or captured"), and SPEC-integrator.harness.md section 7.2 holds the `jitcache-heap: off` directive.
