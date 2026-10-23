# baseline/install.md: `doneLocation` no longer passes into the IC; OSR exit reads the molds

Doc: `skills/reference/knowledge/baseline/install.md`: section "IC initialization", second paragraph; section "BaselineJITData", third paragraph.

## Passages

> Six fields pass through unchanged. [...] `doneLocation` is where the IC's fast and slow paths rejoin the code. The sixth is the name, `m_identifier` [...]

> Installation reads four of the artifact's eleven fields (ten in upstream WebKit): the molds in `m_unlinkedPropertyInlineCaches`, `m_constantPool`, `m_jitCodeMap` for the handlers and `m_isShareable` for parking. The other seven stay in the shared artifact, where their readers find them. [...] OSR exit from the DFG and FTL reads `m_unlinkedCalls` for the return address of an inlined call.

## What changed

`HandlerPropertyInlineCache` has no `doneLocation` field, and `HandlerPropertyInlineCache::initializeFromUnlinkedPropertyInlineCache` copies five fields unchanged: `accessType`, `preconfiguredCacheType`, `propertyIsInt32`, `canBeMegamorphic` and `m_identifier`. The mold (`BaselineUnlinkedPropertyInlineCache`) still has ten fields and keeps `doneLocation`. OSR exit from the DFG and FTL, when it leaves an inlined getter, setter or proxy trap whose caller runs in baseline, finds the return address with `BaselineJITCode::getPropertyInlineCacheDoneLocationForBytecodeIndex`, a linear scan of `m_unlinkedPropertyInlineCaches` by bytecode index (`callerReturnPC` in `DFGOSRExitCompilerCommon.cpp`). So the molds have a reader after installation.

## Proposed text

IC initialization:

> Five fields pass through unchanged. `accessType` picks one of the thirty access types. `preconfiguredCacheType` is the shape the compilation chose: [unchanged]. `propertyIsInt32` says a by-val subscript is a constant int32. `canBeMegamorphic` always arrives false. The fifth is the name, `m_identifier`: [unchanged]. The mold's `doneLocation`, where the IC's fast and slow paths rejoin the code, stays in the artifact.

BaselineJITData, third paragraph: after "OSR exit from the DFG and FTL reads `m_unlinkedCalls` for the return address of an inlined call", add "and the molds' `doneLocation` for that of an inlined getter, setter or proxy trap (`getPropertyInlineCacheDoneLocationForBytecodeIndex`)".
