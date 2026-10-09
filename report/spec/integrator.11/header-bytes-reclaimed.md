mechanical

Requirement. SPEC-integrator.maintenance.md section 4.5: "Each successful unlink adds the body's size to `bytesReclaimed` and counts in `bodiesEvicted`"; section 3, step 3, adds each temporary's size; test M3 has `Yes` delete "temporaries, bodies, header and directories". THREAD Maintenance: `clean` and `compact` "report outcomes, diagnostics and reclaimed bytes".

Why. The SPEC says what temporaries and bodies add to `bytesReclaimed`, but not what a whole-artifact deletion adds for `header`, which it also removes and whose size `postCleanBytes` counts (section 4.2, step 5). Leaving the header out would make a finished deletion report fewer bytes than it removed.

What the code does. Once the `unlinkat` of `header` succeeds, `compact` adds the header file's size to `bytesReclaimed`, so a finished deletion reports `postCleanBytes` plus the temporaries' bytes. `plan.evictedBytes` still counts bodies only, and a deletion that keeps `header` adds nothing for it. Section 4.5 only has to say so. Tests that read the total should expect it: the Bun tests of SPEC-integrator.md section 15.3 for `bun jitcache compact`, and any JS test that parses the summary line.

Evidence. `JSC::JITCache::Maintenance::compact` in `jitcache/JITCacheMaintenance.cpp` adds `HeaderRead::bytes`, the size `readHeader` read; `maintenanceWholeDeletionRemovesEverythingButTheLockFile` in `jitcache/tests/MaintenanceTests.cpp` expects the temporaries, the bodies and the header.
