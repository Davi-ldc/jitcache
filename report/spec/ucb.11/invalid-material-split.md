mechanical

Requirement. SPEC-ucb.md section 13.3, `invalid-material.js`: with strict on, the test covers "each malformed field of sections 4 and 5" and the further cases its row lists, and "Each case is a sequence of its own, picked by the sequence index, whose Producer captures and rewrites the body and whose Consumer, run 1, imports it, so run 1 declares `jitcache-expect-fault` for each step these cases reach".

Why the code cannot meet it as written. One script cannot hold every case. A sequence is one `// jitcache-runs:` line, and the runner reads directives only "in comment lines within its first 50 lines" (SPEC-integrator.harness.md section 7.2). The rules of sections 4.2 and 5.2 that a rewrite of the same size can break, together with the closure and decode cases the row lists, come to 66 cases, so 66 run lines plus the script's other directives.

What the code does instead. Two scripts share the bodies under test, the Producer's capture and rewrite, and the Consumer's check, which `JSTests/jitcache/ucb/resources/invalid-material-cases.js` holds:
- `JSTests/jitcache/ucb/invalid-material.js`: the 37 cases of `ucb.identity` (section 4.2), of the reference closure and of the core, whose run 1 declares the faults `ucb.identity`, `ucb.closure` and `ucb.decode`;
- `JSTests/jitcache/ucb/invalid-material-feedback.js`: the 29 cases of `ucb.feedback` (section 5.2), whose run 1 declares `ucb.feedback`.

Each case is still a sequence of its own, and every other part of the row holds in both scripts. Nothing is asked of other tasks. The SPEC's table would list both scripts.

Evidence.
- The directive window: SPEC-integrator.harness.md section 7.2, which `Tools/Scripts/run-jitcache-tests` implements.
- `obeysIdentityRules` (`jitcache/UCBSections.cpp`) and `obeysFeedbackRules` (`jitcache/UCBFeedback.cpp`), whose rules the cases break one at a time.
