mechanical

Requirement. SPEC-integrator.maintenance.md section 7, test M3: with an unknown file in `bodies/`, a confirmed deletion "returns `Done` with `rmdir-failed`, and the next Producer reuses the remnant." SPEC-integrator.md section 17, task 11: "After tasks 5 and 6."

Why. Only `JITCache::start` reuses a remnant (SPEC-integrator.md section 3.2, step 7; container sub-SPEC section 1.3), and task 7 defines it. No function of tasks 5 or 6 creates an artifact, so the last clause of M3 needs a task that section 17 does not list among task 11's prerequisites. Section 17's rule that "a task calls only functions that it or an earlier task defines" allows the call, since task 7 comes before task 11.

What the code does. `maintenanceWholeDeletionLeavesARemnantTheNextProducerReuses`, the only maintenance test that needs a VM, calls `JITCache::start` with role `Producer` and strict on, and expects `Created`, a new `cache/header` and the unknown file still in `bodies/`. Task 7 has landed in this tree, so the test builds and links now. The backend calls nothing from task 7. Section 17 should list task 7 among task 11's prerequisites; until it does, a run that orders tasks by that list must land task 7 before this test file.

Evidence. `maintenanceWholeDeletionLeavesARemnantTheNextProducerReuses` in `jitcache/tests/MaintenanceTests.cpp`; `start`, `createArtifact` and `resetRemnant` in `jitcache/JITCacheAPI.cpp`.
