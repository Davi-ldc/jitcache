mechanical

Requirement. SPEC-ucb.md section 13.3, `invalid-material.js`. The test goes "through `jitcacheRewriteSection` (SPEC-integrator.harness.md section 5.2), which rewrites a section and reseals the container", and covers among its cases "in `ucb.core` a truncation, ... each resealed".

Why the code cannot meet it as written. `jitcacheRewriteSection` cannot shorten a section. It calls the writer's `rewriteSection`, which "overwrites `bytes.size()` bytes of `kind`'s section at `offset`" and fails at `writer.rewrite` for "a range that does not lie inside the section" (SPEC-integrator.container.md section 8.3). So the body keeps the core's full length. No other helper can commit a shorter `ucb.core` behind a sound container: the harness's other jsc functions (section 5.2) read sections and never write them.

What the code does instead. The case makes the decode find the payload ending before its root record. It writes the root record's offset, at bytes 8 to 11 of the core, as the payload's length rounded up to 4. Section 3 of SPEC-ucb.codec.md then rejects it, because "the record does not lie inside the payload". That is the check a truncated core meets: it fails `Malformed` at `ucb.decode`, the step the case expects. Every other byte of the core keeps its value. The case is "a truncated payload" in the table of `JSTests/jitcache/ucb/invalid-material.js`, which `JSTests/jitcache/ucb/resources/invalid-material-cases.js` runs. Nothing is asked of other tasks. If the SPEC wants a truly shorter section, the harness would need a rewrite that may change a section's size, which is the integrator's to add.

Evidence.
- `rewriteSectionForTesting` (`jitcache/JITCacheCapture.cpp`) and `ArtifactWriter::rewriteSection` (`jitcache/ArtifactWriter.cpp`).
- `functionJITCacheRewriteSection` (`jitcache/JITCacheTwinsHarness.cpp`).
- `coreCodecDecodeRoot` (`runtime/CachedTypes.cpp`), which `decodeUnlinkedCodeBlockCore` calls: a root offset past the payload, or one that leaves fewer bytes than the record, fails `Malformed`.
