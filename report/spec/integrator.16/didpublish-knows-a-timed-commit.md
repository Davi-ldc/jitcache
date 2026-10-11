mechanical

Requirement. SPEC-integrator.md section 6.3 declares the writer's private member for step 8 as `void didPublish(const BodyKey&, uint64_t inode);`. Container sub-SPEC section 5.2 declares, in `IndexStatistics`, `timedWriterUpdates` as the updates "of a commit given a CommitTiming, the only ones that read a clock", with `writerUpdateNanoseconds` beside it. Harness sub-SPEC section 9.2 writes these in the `index` line as `writerUpdates.timed` and `writerUpdates.nanoseconds`. The requirement has no rule ID.

Why the code cannot meet it as written. Step 8 runs inside `didPublish`, under `m_indexLock`, and only `didPublish` can time its own work under that lock. With the declared signature it cannot tell whether the commit was given a `CommitTiming`. It would have to read the clock on every commit, which section 5.2 rules out for untimed commits, or never read it, which leaves both fields at zero.

What the code does instead. The member is `void didPublish(const BodyKey&, uint64_t inode, bool timed)`, and `ArtifactWriter::commit` passes `!!timing`. It counts every update in `writerUpdates`. When `timed` holds, it also reads `MonotonicTime::now()` at the start and end of its work under the lock and adds the result to `timedWriterUpdates` and `writerUpdateNanoseconds`. The member is private and has one caller, so no other task is affected. The SPEC's declaration should gain the parameter.

Evidence. `ArtifactWriter::didPublish` and `ArtifactWriter::commit` in `jitcache/ArtifactWriter.h` and `jitcache/ArtifactWriter.cpp`; `IndexStatistics` in `jitcache/ArtifactStore.h`; the test `writerCommitTimingCoversItsParts` in `jitcache/tests/WriterTests.cpp`, which checks that an untimed commit counts its update without timing it.
