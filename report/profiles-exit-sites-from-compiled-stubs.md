# profiles.md: exit sites are tallied from the exits whose stubs were compiled

Doc: `skills/reference/knowledge/common/profiles.md`, the `DFG::ExitProfile` row of the table.

## Passage

> `CodeBlock::tallyFrequentExitSites`, which `CodeBlock::jettison` calls when the jettisoned block is still the executable's replacement, for every reason except old age and VM traps, whether the GC or the running code jettisons; each exit with a nonzero count adds its site to the UCB of the exit origin's baseline block [...]

## What changed

Exits no longer carry a count: `handleExitCounts` does not increment `exit.m_count`, which is gone. `CodeBlock::tallyFrequentExitSites` walks the exit stubs that were compiled instead: `dfgJITData()->exitStubs()` for the DFG and `FTL::JITCode::m_osrExitStubs` for the FTL. A stub is compiled, and appended there, the first time its exit is taken (`DFG::JITData::appendExitStub`, which asserts that each exit index appears once; in the FTL, `FTLOSRExitCompiler.cpp` appends after `compileStub`). Each listed exit calls `OSRExitBase::considerAddingAsFrequentExitSite`, which skips kinds for which `exitKindMayJettison` is false and otherwise adds the site, or `HoistingFailed` for a hoisted exit, to the exit origin's baseline block.

## Proposed text

Replace "each exit with a nonzero count adds its site" with:

> each exit whose stub has been compiled, which happens the first time the exit is taken (`exitStubs()` in the DFG, `m_osrExitStubs` in the FTL), adds its site, if its kind may jettison (`exitKindMayJettison`),
