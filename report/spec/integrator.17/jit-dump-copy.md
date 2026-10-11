mechanical

## Requirement

SPEC-integrator.harness.md N19: "`LinkBuffer::finalizeCodeWithoutDisassemblyImpl` passes every finalized allocation's final bytes to `PerfLog::log` under `useJITDump`, which writes them with the allocation's name as perf `JIT_CODE_LOAD` records". Section 11.3, Targets: "On x86_64 the tool therefore reads the displacement of every `call`, `jmp` and conditional jump with a 32-bit displacement from the JIT dump's bytes at that instruction, in the record of the body's own allocation."

## Why the code cannot meet it as written

`PerfLog::log` does not copy the bytes when it is called. It dispatches the copy to `ProfilerSupport`'s "JSC PerfLog" queue, which reads the code's address whenever that queue runs. Meanwhile the VM thread goes on running the body. `JITMathIC::generateOutOfLine`, through its `linkJumpToOutOfLineSnippet`, can overwrite the first bytes of a MathIC's inline region with a jump to its snippet before the copy happens. With an empty profile the inline region is a single `jmp rel32` to the slow path. Its record bytes then name the snippet in one run and the slow path in the other, depending on timing.

The records do reach the file at exit: `ProfilerSupport`'s constructor registers an `atexit` handler that calls `barrierSync`.

## What the code does instead

For a branch whose displacement field is covered by an inline rewrite from the same run, `RunContext.readDisplacements` keeps the disassembly's target. An inline rewrite is an allocation whose header starts inside the code of an earlier baseline body. Its header prints `[p, p) 0 bytes:`, because its `LinkBuffer` writes into the body and owns no memory, so the tool takes its extent from its listing: from p to the end of its one jump, which `jumpThunk` emits as the 5-byte near `jmp` on x86_64. Every other `call`, `jmp` and conditional jump takes its displacement from the dump.

The disassembly's target is the right one for such a branch. The body is dumped before any rewrite, and a MathIC inline region jumps only within its own body, so the branch is already linked when the dump prints it.

Other tasks need do nothing. N19 should say that the queue copies the bytes when it runs.

## Evidence

- `PerfLog::log` (`assembler/PerfLog.cpp`).
- `ProfilerSupport::ProfilerSupport` and `ProfilerSupport::barrierSync` (`runtime/ProfilerSupport.cpp`).
- `JITMathIC::generateOutOfLine` (`jit/JITMathIC.cpp`).
- `JITAddGenerator::generateInline`, whose empty profile emits a jump to the slow path.
- `LinkBuffer::finalizeCodeWithoutDisassemblyImpl`, which returns a self-managed `CodeRef` of size 0 for a `LinkBuffer` built over existing code, and `PerfLog::log`, which writes no record for size 0.
- `RunContext.readDisplacements` and `RunContext.listingEnd` in `Tools/Scripts/jitcache-pin-compare.ts`.
- The crafted pairs "a JIT dump copied after an inline rewrite", "an inline rewrite to another place in its snippet" and "a JIT dump branch no rewrite explains" in `JSTests/jitcache/integrator/runner/pin-compare-tests.ts`.
