mechanical

Requirement. SPEC-integrator.md section 3.2, step 10: the new `VMState` "keeps the bench report of step 3, hands it the `OpenedArtifact` of step 7 with `setArtifact`, whose statistics each flush's `index` line reads". Section 17 lists, under task 16, "the `index` line with the store's `IndexStatistics` (`ArtifactStore.h`, `ArtifactStore.cpp`) and the `setArtifact` call in `start`'s step 10 (`JITCacheAPI.cpp`)". Harness sub-SPEC section 9.2 writes the `index` line "at each flush, when the state handed the report an artifact (`setArtifact`)". The requirement has no rule ID.

Why the code cannot meet it as written. The workflow assigns task 16 a fixed set of files, and `JITCacheAPI.cpp`, which defines `start` and the state's constructor, is not one of them. This task therefore cannot write the call.

What the code does instead. Task 16 writes everything on the report's side. `JITCacheBench.h` declares `BenchReport::setArtifact(RefPtr<OpenedArtifact>&&)` and the member `RefPtr<OpenedArtifact> m_artifact`, and `JITCacheBench.cpp` defines the setter. `BenchReport::flush` writes one `index` line from `m_artifact->statistics()` when the report holds an artifact and writes none when it does not. `ArtifactRegistry::openedArtifacts()` and the flush's walk over it are gone. Until the call lands, no report holds an artifact, so no flush writes an `index` line.

The call lands separately, in `JITCacheAPI.cpp`. Step 10 runs in `VMState::VMState(const Config&, StartParts&&)`, which already calls `m_benchReport->setBudget`. The call goes after the member initializers and before the early return for a state without a producer limit, so that Consumers get it too:

```cpp
    if (m_benchReport && m_artifact)
        m_benchReport->setArtifact(m_artifact.copyRef());
```

`m_artifact` is null only after a start fault, and then the report writes no `index` line, as harness sub-SPEC section 9.2 says. The report holds its own reference, so the artifact outlives every flush, including the one `willDestroyVM` runs. `~BenchReport` writes no summary lines and reads no statistics.

Evidence. `BenchReport::setArtifact`, `BenchReport::m_artifact` and `BenchReport::flush` in `jitcache/JITCacheBench.h` and `jitcache/JITCacheBench.cpp`; `VMState::VMState` and `start` in `jitcache/JITCacheAPI.cpp`; `OpenedArtifact::statistics` in `jitcache/ArtifactStore.h` and `jitcache/ArtifactStore.cpp`.
