design

SPEC-integrator.container.md, section 6.2, the listing: "every body name's key with the inode `readdir` reports for it (`d_ino`), temporaries, `.` and `..` ignored, and any other name counted for `status`."

`status` has no field for that count: SPEC-integrator.md declares `Progress` without one, and no section says what `status` would report. Task 5 counts the names into `OpenedArtifact::m_foreignNames` (`ArtifactStore.cpp`, `OpenedArtifact::list`), read only by two checks of `storeListingKeepsTokens` in `tests/StoreTests.cpp`.

Whether `status` reports the count is a product question, parked for Davi (docs/JitCache/drafts/spec-drain-1.md, D8 and "Parked for Davi"). The recommendation is no: the listing ignores every name that is not a body, `clean` stays the one report of stray names (maintenance sub-SPEC section 3, `unknown-file`), and task 5's counter goes. Until Davi decides, nothing changes: integrator.7 implements `status` with the fields `Progress` declares, and the store keeps its counter.
