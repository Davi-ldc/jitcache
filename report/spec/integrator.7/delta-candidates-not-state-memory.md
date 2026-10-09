mechanical

SPEC-integrator.md, section 4.2: "Production memory (kept summaries, the writer's staging buffer, delta's candidates, and the charge for the index entries the writer added, section 4.4) is released by `releaseEndedProductionMemory()`".

The state never holds a candidate table. Section 4.1's table of the state's parts has no row for one. Section 8.6 builds the table inside one `delta` call (step 3) and frees it in that same call (step 5). When production ends, no candidate table is left for `releaseEndedProductionMemory` to release.

`VMState::releaseEndedProductionMemory` releases the three parts the state does hold: the kept summaries through the deleter the capture glue supplies, the writer's staging buffer through `ArtifactWriter::releaseStagingBuffer`, and the index-entry charge that `VMState::addIndexEntryCharge` accumulated. For II4 to hold, task 9 must free the candidate table and release its charges before every return of `delta`, the `Faulted` returns of section 8.6 steps 3 and 4 included, not only at step 5. The SPEC only has to say that `delta` frees its own candidates.

Evidence: `VMState::releaseEndedProductionMemory` in `Source/JavaScriptCore/jitcache/JITCacheAPI.cpp`; the private members `m_keptSummaries`, `m_indexEntryChargeBytes` and `m_writer` of `VMState` in `Source/JavaScriptCore/jitcache/JITCacheVMState.h`.
