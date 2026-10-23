# inlinecaches.md: `HandlerPropertyInlineCache` has no `doneLocation`; OSR exit reads the artifact's molds

Doc: `skills/reference/knowledge/common/inlinecaches.md`, "Property inline cache": section "Layout and the first test", first paragraph, and section "Readers", first paragraph.

## Passages

> Besides `doneLocation`, `codeOrigin`, `callSiteIndex`, the identifier and the `JSGlobalObject`, its fields fall into five groups:

> When a DFG or FTL frame exits inside an inlined getter, setter or proxy trap whose caller runs in baseline, the return address is the `doneLocation` of the baseline IC at the call's bytecode index (`callerReturnPC`, with a `RELEASE_ASSERT` that the IC exists).

## What changed

`HandlerPropertyInlineCache` (`PropertyInlineCache.h`) has no `doneLocation` field. `RepatchingPropertyInlineCache`, the FTL's, keeps one, and so does the artifact's mold, `BaselineUnlinkedPropertyInlineCache` (`bytecodeIndex`, `doneLocation`). For the getter, setter and proxy cases, `callerReturnPC` (`DFGOSRExitCompilerCommon.cpp`) calls `BaselineJITCode::getPropertyInlineCacheDoneLocationForBytecodeIndex`, which scans the artifact's `m_unlinkedPropertyInlineCaches` for the bytecode index and returns the mold's `doneLocation`, null when there is none. The `RELEASE_ASSERT` is on that result. Because the molds belong to the shared `BaselineJITCode`, the answer is the same for every `CodeBlock` the artifact serves.

## Proposed text

Layout: "Besides `codeOrigin`, `callSiteIndex`, the identifier and the `JSGlobalObject`, its fields fall into five groups:" (the code location after the access stays in the artifact's mold).

Readers:

> When a DFG or FTL frame exits inside an inlined getter, setter or proxy trap whose caller runs in baseline, the return address is the `doneLocation` of the artifact's mold for the call's bytecode index (`callerReturnPC`, through `BaselineJITCode::getPropertyInlineCacheDoneLocationForBytecodeIndex`, with a `RELEASE_ASSERT` that it exists).
