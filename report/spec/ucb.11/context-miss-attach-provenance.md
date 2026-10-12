mechanical

Requirement. SPEC-ucb.md section 13.3, `context-miss.js`, the second sequence: "the reverse, decoded in the producer and generated in the consumer: every program and function import, and every attach to a generated UCB, misses `Provenance`, so that consumer installs nothing (`jitcache-expect-no-install: 1:1`)".

Why the code cannot meet it as written. In that Consumer, no attach to a generated UCB can reach the provenance comparison (section 7.3.2 step 7). The Consumer generates a UCB only after its import attempt at the same request missed, and that attempt reads the same body: it misses `Provenance` at section 7.3.1 step 4b and stamps the index token (section 7.3.1 step 4 and section 7.3.2's paragraph after its steps). `didGenerate` copies the stamp into the generated UCB's record (section 7.3.5). A later CodeCache hit or filled-slot request for that UCB then stops before the match:
- at section 7.3.4 step 2, while the UCB's sharing slot holds the code a native baseline compile parked there, which every hot body of the test has;
- at step 3 once that code is released, where `missedBodyVersion` equals the current token and the miss is `BodyUnchanged`, which reads nothing.
Nothing else in a Consumer-only run changes that body's token, so the stamp holds until the run ends.

What the code does instead. `JSTests/jitcache/ucb/context-miss.js` checks the two halves the code can show. Each program and function import misses `Provenance` and imports nothing. A second evaluation of a program, whose CodeCache hit meets the generated UCB, attaches nothing, misses no `Provenance` and computes no context or source digest. The script's sequence 1 starts with an `Off` run that writes the bytecode cache the Producer decodes, since the jsc shell writes the cache only when the provider dies, so that Consumer is run 2 and the directive reads `jitcache-expect-no-install: 1:2` where the row writes `1:1`. The SPEC row would read "every program and function import misses `Provenance`, and no attach to the UCB it generates follows". Nothing is asked of other tasks.

Evidence.
- `importBody` and `attachLive` (`jitcache/UCBImport.cpp`): the stamp at a provenance miss, and the sharing-slot and `missedBodyVersion` tests that come before any match.
- `UCBRegistry::setMissedBodyVersion` and `UCBRegistry::recordCodeBlock` (`jitcache/UCBRegistry.cpp`).
- `UnlinkedCodeBlock::m_unlinkedBaselineCode` (`bytecode/UnlinkedCodeBlock.h`), set when a native baseline compile parks its code, and `Heap::releaseUnusedSharedBaselineCode` (`heap/Heap.cpp`), which releases it.
