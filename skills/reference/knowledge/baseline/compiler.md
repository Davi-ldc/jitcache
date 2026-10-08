# Abstract

The baseline JIT compiles one `CodeBlock` with no intermediate representation: one pass over the bytecode emits the fast paths, and a second pass over the recorded slow cases emits the slow paths. Each opcode becomes a template. A failed guard jumps to an out-of-line slow path that calls the runtime and rejoins the fast path, so execution stays in the same code: there is no OSR exit. Some templates shape their fast path by what the LLInt and the profiles have observed, and a few opcodes are only a call to their runtime function. The output is a `BaselineJITCode`: the machine code with its two entry points, plus side tables that locate points in the code and describe the per-`CodeBlock` state that installation builds.

One compilation can serve every `CodeBlock` of the same `UnlinkedCodeBlock` in the same `VM`. The state that differs between those `CodeBlock`s (inline caches, function executables, profiles, LLInt caches, scope metadata, the global object) is reached at run time through the frame's `CodeBlock`, almost always through two base registers that the function entry loads from it. The rest is fixed in the bytes: addresses owned by the `UnlinkedCodeBlock`, the `VM` and the process, plus a few facts about the `CodeBlock` that was compiled.

# Trigger and threads

A `CodeBlock` is compiled in two situations. The usual one is the LLInt's tier-up. Its counter lives in the `UnlinkedCodeBlock`, so every `CodeBlock` of the body (the code one `UnlinkedCodeBlock` describes) adds to it, and when it crosses the threshold the slow path `jitCompileAndSetHeuristics` runs under `DeferGCForAWhile`, after the `shouldJIT` gate (`useBaselineJIT`, `bytecodeRangeToJITCompile`, `jitAllowlist`). If another `CodeBlock` of the body already parked an artifact in the `UnlinkedCodeBlock`, the slow path installs it without compiling. Otherwise, once `checkIfJITThresholdReached` confirms the threshold, it finalizes the `VM`'s ready plans and enqueues a `BaselineJITPlan` for this `CodeBlock` unless the worklist, which keys baseline plans by the `UnlinkedCodeBlock`, knows one for the body, in flight or just finalized. A body thus has at most one baseline compilation in flight; a `CodeBlock` turned away picks up the parked artifact at a later crossing. With `useConcurrentJIT` off, which `useProfiler` and `forceEagerCompilation` also force, `JITWorklist::enqueue` compiles and finalizes on the calling thread before it returns.

The other situation is a disabled LLInt. `prepareForExecutionImpl` then compiles each new `CodeBlock` at birth through `JIT::compileSync` with `JITCompilationMustSucceed`, without the `shouldJIT` gate, unless its `UnlinkedCodeBlock` already holds an artifact.

The compilation (`BaselineJITPlan::compileInThreadImpl`) first drains the value profiles and, except in builtins, merges their predictions into the `UnlinkedCodeBlock`'s copies; then it opens one `Safepoint` that lasts until the artifact exists (`JIT::compileAndLinkWithoutFinalizing`). On a worklist thread the safepoint releases `m_rightToRun`, so the collector can stop the world and run while the compilation continues; a baseline plan reports itself live to every collection until it is canceled, which keeps its `CodeBlock` marked. On the requesting thread the safepoint releases nothing, and the collector cannot stop the world before the compilation ends. With `useLOLJIT` on (off by default) the plan runs `LOL::LOLJIT` instead of `JIT`; this document covers `JIT`.

# Inputs

The compilation reads:

| input | where from | what it decides in the code |
|---|---|---|
| bytecode | UCB, `m_instructions` | the stream the main pass walks |
| constants | UCB, `m_constantRegisters` | constants the UCB owns go in as immediates, a cell as its pointer. `LinkTimeConstant`, `SymbolTable` and `JSTemplateObjectDescriptor` constants belong to each `CodeBlock` and are loaded at run time (`CodeBlock::isConstantOwnedByUnlinkedCodeBlock`). NaN constants arrive canonical: the bytecode generator stores every NaN as `jsNaN()` |
| identifiers | UCB, `m_identifiers` | the property name in the IC molds of `get_by_id`, `get_by_id_direct`, `get_by_id_with_this`, `put_by_id`, `in_by_id` and `del_by_id`, and the `StringImpl*` that `put_getter_by_id`, `put_setter_by_id` and `put_getter_setter_by_id` pass to their operations |
| numeric, character and string jump tables | UCB | each switch's form, range, targets and case atoms (see Switches below) |
| arithmetic profiles | UCB | the fast path each arithmetic template emits, or a patchable jump when the profile is empty |
| frame shape | UCB | frame size, how many locals `op_enter` clears, whether there is an arity-check entry and argument profiling |
| function declarations and expressions | UCB | the operation `new_func` and `new_func_exp` call, chosen by each `UnlinkedFunctionExecutable`'s builtin, arrow and strict bits (`selectNewFunctionOperation`) |
| metadata layout | UCB, through the `CodeBlock`'s `MetadataTable` | each opcode's metadata offset, baked as a displacement |
| LLInt caches and scope metadata | `CodeBlock` metadata: `m_modeMetadata`, `m_enumeratorMetadata`, `m_resolveType`, `m_localScopeDepth`, `m_getPutInfo` | which variant of the template comes out |
| `capabilityLevel()` | `CodeBlock` | whether profile writes and tier-up counters are emitted |
| `couldBeTainted()` | `CodeBlock` | whether `op_enter` stores the taint flag |
| `TypeLocation`, `BasicBlockLocation` | `CodeBlock` metadata, pointing into the `VM`'s profilers | the last type `profile_type` saw picks an inline check; the addresses go into the code |
| `VM*`, field addresses, thunks | `VM` | see [What the code embeds](#what-the-code-embeds) |
| options, CPU features, `structureIDBase` | process | see below and [What the code embeds](#what-the-code-embeds) |
| random draws | the assembler's generator, and `BinarySwitch`'s own | the entry's `nop` and constant blinding; the order of `BinarySwitch` tests ([What the code embeds](#what-the-code-embeds)) |

The LLInt keeps writing these caches while a worklist thread reads them, which is safe: shapes chosen from values that can still change are guarded at run time, and the unguarded ones are fixed at link ([Sharing between CodeBlocks](#sharing-between-codeblocks)).

Whether a string constant is an atom also changes over time. `stricteq`, `nstricteq`, `jstricteq` and `jnstricteq` compare inline against a constant string only when its `StringImpl` is an atom; otherwise they emit the generic comparison, which takes the slow path when both sides are strings. Generation makes every string constant from an `Identifier`, so all are atoms. The bytecode cache's decoder makes a constant longer than an inline string a plain string (`decodePlainString`) unless it already holds that string's atom. Later, using the constant as a property key (`JSString::toIdentifier`, `toAtomString`) or the decoder's string table promoting it (`DecoderStringTable::atomFor`) turns its value into the atom in place (`swapToAtomString`), and nothing turns it back. Two compilations of one decoded body can therefore take the fast path at different comparisons. Both are correct, and the `CodeBlock`s sharing an artifact share these constants through the `UnlinkedCodeBlock`. A swap during a worklist compilation is harmless too: the old value stays alive, and either answer yields correct code.

## Profiling and tier-up counters

`capabilityLevel()`, read once at the start, decides whether the code feeds the next tier. It is `CannotCompile` when `useDFGJIT` is off, when the bytecode cost exceeds `maximumOptimizationCandidateBytecodeCost` (100,000), or when the executable carries `setNeverOptimize`, a flag only test code sets in the engine (`CodeBlock::computeCapabilityLevel`). From it the JIT sets two booleans that always agree in this pin. `m_shouldEmitProfiling` controls the value, array and argument profile writes, and the arithmetic ones of `div`, `to_number` and `to_numeric`. `m_canBeOptimized` controls the tier-up counter increments of `op_enter` and `loop_hint`, which add the immediates `executionCounterIncrementForEntry` (15) and `executionCounterIncrementForLoop` (1), and the `operationOptimize` call in `loop_hint`'s slow path. The per-`VM` `op_enter` handler thunk, which `op_enter`'s slow path calls for traps and for the `CodeBlock`'s write barrier, adds the entry increment and may call `operationOptimize` whenever `useDFGJIT` is on, so code compiled with the flag off still counts the entries that take that slow path. `operationOptimize` rereads `capabilityLevel()` and returns at once for `CannotCompile`.

Other feedback ignores both booleans: the catch buffer's value profiles (`operationTryOSREnterAtCatchAndValueProfile`), the `ArrayAllocationProfile` that `new_array` and `new_array_with_size` pass, the largest varargs count kept in the `CallLinkInfo`, the enumerator and iteration modes, and `jneq_ptr`'s `m_hasJumped`. With the flag off, the Math ICs of `add`, `sub`, `mul` and `negate` still write their arithmetic profile: their full snippets, inline or regenerated, set result bits, and the regenerating slow operation records operand types until its call is repointed to a variant that records nothing; a site with no fast path at all calls an operation that records nothing. With the flag on, the slow operations are the profiled variants, which record operands and result.

## Options and CPU features

Most options act around the compilation: the `shouldJIT` and `capabilityLevel()` options above, and `useLLIntICs`, which decides whether the LLInt fills the property caches the `get_by_id` shape comes from. `jitPolicyScale` and `forceEagerCompilation` rewrite other options before any compilation. Options live in the process's `JSCConfig`, writable until the first `VM` exists and locked with `mprotect` after that.

These change the code itself, almost always by adding instructions:

| option | default | effect on the code |
|---|---|---|
| `forceGCSlowPaths` | false | every inline allocation becomes a jump to its slow path |
| `useJITAsserts` | `ASSERT_ENABLED` | debug builds only: checks at the function entry (tag registers, argument count, a baseline frame `CodeBlock` owned by the callee), on the doubles the arithmetic generators box and unbox, and on the operands of `jbelow` and `jbeloweq` |
| `useJITDebugAssertions` | `ASSERT_ENABLED` | a `breakpoint()` check in the global property path of `put_to_scope` and of the `get_from_scope` thunk, in release builds too |
| `eagerlyUpdateTopCallFrame` | false | each bytecode stores its call site index into the frame, as every operation call already does |
| `maximumInlineStringSwitchCaseCount` | 64 | the largest `switch_string` dispatched inline |
| `traceBaselineJITExecution` | false | a `probeDebug` on each bytecode and each slow path, carrying a pointer to a heap-allocated callback |
| `useProfiler` | false | the `VM` creates a `Profiler::Database`, and each bytecode increments its execution counter there by absolute address |
| `useExceptionFuzz` | false | each exception check that reads the `VM`'s exception word first saves all registers into a `VM` buffer, calls `operationExceptionFuzzWithCallFrame` with the `VM*` through a register and reloads them (ARM64 also saves and restores the frame pointer and link register). In compiled code that is only the check after an operation that returns a register pair; most of these checks live in thunks, `checkException` among them |
| `returnEarlyFromInfiniteLoopsForFuzzing` | false | in eligible bodies, `loop_hint` loads a per-`VM` counter keyed by the instruction's address, returns `globalThis` once it passes `earlyReturnFromInfiniteLoopsLimit`, and stores it back incremented |

The last four put addresses into the code that exist only in the compiling process. Builds with `ASSERT_ENABLED` differ even with every option at its default: each bytecode except `op_catch` calls a consistency check of the base registers and the stack pointer, each operation call stores the frame into `vm.topCallFrame`, and an operation that returns its exception in a register has that register compared with `vm.exception`.

CPU features choose instructions too. On ARM64, `JITRightShiftGenerator` emits `fjcvtzs` when FEAT_JSCVT is present and otherwise a truncation path with one more slow case, so the feature changes the set of slow paths a bytecode leaves. On x86_64, double arithmetic and conversions pick AVX or SSE forms at emission, and on Linux `collectCPUFeatures` reports AVX only when the processor has it and the operating system saves the XMM and YMM state.

# Emission

## Reaching per-CodeBlock state

The `CodeBlock` sits in a fixed slot of every call frame. The function entry loads it and fills both base registers with one `loadPairPtr` from two adjacent `CodeBlock` fields: `jitDataRegister` gets the `BaselineJITData` and `metadataTableRegister` the `MetadataTable`. Nearly every access to per-`CodeBlock` state is one of these registers plus a constant; the last row is the state shared by all `CodeBlock`s of the body:

| state | how the code reaches it |
|---|---|
| property IC number `i` | `jitDataRegister - (i + 1) * sizeof(HandlerPropertyInlineCache)`: the ICs sit before the `BaselineJITData` object |
| function executable number `i` | `jitDataRegister + offsetOfTrailingData() + i * sizeof(void*)`: the constant pool sits after the object |
| global object, tier-up counter | fields of the `BaselineJITData` object |
| an opcode's metadata: LLInt caches, array and allocation profiles, `CallLinkInfo`, scope metadata | `metadataTableRegister` plus that opcode's metadata offset, or a register holding the sum when the offset does not fit the addressing mode |
| value profiles | `metadataTableRegister` minus an offset: they sit before the table in the same allocation |
| argument profiles, constants the `CodeBlock` owns | the frame's `CodeBlock`, loaded again |
| arithmetic profiles, Math ICs, profiler entries | absolute addresses in the code ([What the code embeds](#what-the-code-embeds)) |

## Stages

1. Function entry. A random draw decides whether a `nop` comes first. Then come the prologue, the stack check against the `VM`'s soft stack limit, the callee saves, the tag registers and the two base registers. When profiling is on, function code copies each argument into its argument profile, skipping `this` in a constructor; a constructor with no declared parameters gets no copies at all.

2. Main pass. The JIT walks the bytecode in order and calls each opcode's emitter (`emit_op_add`, `emit_op_get_by_id`), recording in `m_labels` where each instruction starts. A jump to another bytecode (a JS branch, a loop back edge, a case of a binary-search switch) goes into `m_jmpTable` with its target offset, even when the target is behind and already emitted. A guard that can fail (a type test, an overflow) usually goes into `m_slowCases`, tagged with the current bytecode. The by-id ICs keep their structure-mismatch jumps in their generator and leave only a placeholder there, and a few opcodes, such as `enumerator_next` and `switch_string`, call their slow operation inside the main pass. Slow paths all go after the fast paths, which keeps the fast paths contiguous and spares a jump over each slow path. Opcodes without a template (`DEFINE_SLOW_OP` in `JIT::privateCompileMainPass`, such as `typeof`, `strcat` and `spread`) become a call to a thunk that runs their `slow_path_*` function.

3. Jump resolution. With every position known, `privateCompileLinkPass` links each `m_jmpTable` entry to its target's label. On x86_64 this writes the displacement into the buffer; on ARM64 it records a link that the copy into executable memory resolves.

4. Slow paths. `privateCompileSlowCases` walks `m_slowCases` and emits each bytecode's slow path, through its `emitSlow_` function or, when the slow path is a plain call, through `emitSlowCaseCall` and the `slow_path_*` thunk, followed by a jump back to the fast path's resume point. A slow path that branches to another bytecode links straight to `m_labels`, since every label exists by then. Release assertions check that each slow emitter consumes exactly its own jumps, that the number of bytecodes with slow cases matches the main pass, and, for eight of the twelve IC generator families (all but `get_by_val`, `in_by_val`, `del_by_id` and `del_by_val`), that every generator was consumed. After the slow paths come the consistency-check routine of debug builds, the arity-check entry and the stack overflow exit, which jumps to the `VM`'s `ThrowStackOverflowAtPrologue` thunk; an overflow found by the arity-check entry, before any prologue ran, first passes through a second prologue. The arity-check entry exists only for function code with at least one declared parameter: it jumps to the normal entry, first calling `LLInt::arityFixup()` when the caller passed fewer arguments than parameters. Without it, both entry points are the same address.

## Templates that leave records

Most templates produce only code. The ones below also leave records that linking completes and the artifact keeps.

Property accesses. Twenty-five emitter sites each create a mold (`BaselineUnlinkedPropertyInlineCache`) through `addUnlinkedPropertyInlineCache`: the access type, the shape the compilation chose, the property name when it is a constant, whether a by-val subscript is a constant int32 (`propertyIsInt32`), the bytecode index and, at link, `doneLocation`, where the access rejoins the fast path. The name is an identifier of the `UnlinkedCodeBlock`, except in `get_length` and in the steps inside `iterator_open`, `iterator_next` and `instanceof`, which use immortal `VM` identifiers (`length`, `next`, `done`, `value`, `Symbol.hasInstance`, `prototype`). Installation builds IC number `i` from mold `i`.

The eleven sites of the `JITByIdGenerator` family test inline first: `get_by_id`, `get_by_id_direct`, `get_length`, `get_by_id_with_this`, `put_by_id`, `in_by_id`, one step of `iterator_open` (a site `async_iterator_open` shares), two of `iterator_next` and two of `instanceof`. The test compares the base's `StructureID` with the IC's inline structure field and then performs the access: a load from the base or, for a prototype access, from the IC's holder; a store for `put_by_id`; just `true` for `in_by_id`. `get_length` tests the array's indexing type instead. On a mismatch, and at the other fourteen sites always, the code calls through the IC's handler chain. The templates' other guards (a base that is not a cell and, in `get_by_id_with_this` and `del_by_val`, a second operand that is not a cell) go to the `VM`'s slow-path thunk for the access type (`InlineCacheCompiler::generateSlowPathCode`); the steps inside `iterator_open`, `iterator_next` and `instanceof` throw instead. The shape comes from the LLInt: `get_by_id`, `iterator_open` and `async_iterator_open` read `m_modeMetadata` and choose `GetByIdPrototype` for `ProtoLoad` and `GetByIdSelf` otherwise.

Calls. Each call opcode keeps its `CallLinkInfo` in the opcode's metadata, shared with the LLInt. Except in `call_direct_eval`, which first calls an operation and only falls back to a virtual call, the fast path loads the expected callee, the target and the callee's `CodeBlock` from it. When the callee matches or the cache is polymorphic, it calls the stored target (tail calls jump to it); otherwise it calls `LLInt::defaultCall()`. Each of these call sites also leaves a `BaselineUnlinkedCallLinkInfo` with its bytecode index, and linking fills in `doneLocation`, the return point (for a tail call, the label after the jump). `op_ret` jumps to the `ReturnFromBaseline` thunk.

Math ICs. `add`, `sub`, `mul` and `negate` each create a `JITMathIC` bound to the site's arithmetic profile in the `UnlinkedCodeBlock`. `generateInline` looks at that profile: an empty one yields a patchable jump to the slow path; otherwise the generator emits a specialized fast path, padded with nops to the size of a jump so the region can later be overwritten with one, or the full snippet, or nothing, in which case the template just calls the operation. The slow path passes the IC's address, or the profile's, as an immediate to the slow operation, and a link task records four locations in the code: inline start and end, slow-path start and slow call. On a miss the operation calls `generateOutOfLine` (at most twice per IC), which compiles a snippet into a separate allocation, rewrites the inline region in place with a jump to it and, on the last generation, repoints the slow call to a variant that no longer regenerates. The code therefore changes after linking.

Switches. A `switch_imm` or `switch_char` whose table is a list becomes a binary search (`BinarySwitch`). A dense one subtracts the minimum, checks the bounds and jumps through the `SimpleJumpTable`. `switch_string` with 1 to 64 cases runs another `BinarySwitch` over the case atoms' addresses when the scrutinee is an atom, sending an atom that matches no case to the default target and any other scrutinee to `operationSwitchStringWithUnknownKeyType`, which resolves the target from the artifact's `StringJumpTable`; larger switches only make that call. Each switch leaves a `SwitchRecord` that linking uses to fill in its table.

Function creation. The eight `new_*func*` opcodes load their `FunctionExecutable` from the `BaselineJITData` constant pool. `addToConstantPool` records each slot as a pair (declaration or expression table, index); installation fills it with the executable the `CodeBlock` built in `finishCreation`.

Profilers. `op_profile_type` and `op_profile_control_flow`, present only when the type or control-flow profiler was on at bytecode generation, write profiler addresses into the code and clear `m_isShareable`.

## What the JIT object accumulates

| structure | written in | fate |
|---|---|---|
| `m_labels` | stage 2 | becomes `m_jitCodeMap` at link |
| the twelve IC generator vectors | stages 2 and 4 | fill `doneLocation` in the molds at link |
| `m_unlinkedPropertyInlineCaches` (the molds), `m_unlinkedCalls`, `m_constantPool` | stage 2 | moved into the artifact |
| `m_callCompilationInfo` | stage 2 | fills each call record's `doneLocation` at link |
| `m_switches` | stage 2 | fills the switch tables at link |
| `m_switchJumpTables`, `m_stringSwitchJumpTables` | allocated before stage 1, sized in stage 2, filled at link | moved into the artifact |
| `m_farCalls` | stages 2 and 4 | link each operation call at link |
| `m_mathICs` | created in stage 2, slow calls in stage 4, locations at link | adopted by the artifact |
| `m_pcToCodeOriginMapBuilder` | stages 1, 2 and 4 | becomes `m_pcToCodeOriginMap`, when built |

# Linking

1. Copy. The `LinkBuffer` pads the code with breakpoints to the 32-byte allocation granule, allocates executable memory and copies the code in. On ARM64 the copy also compacts branches: the assembler reserves two instructions per conditional branch (an inverted condition over a `b` reaching ±128 MB), and the copy keeps one wherever the target is in range, ±1 MB for `b.cond` and `cbz` and ±8 KB for `tbz` (JSC's limit; the instruction reaches ±32 KB). Later code moves back, the shifts are recorded over the old buffer so positions can still be translated, and the allocation shrinks to the compacted size. Distances to thunks are measured to their absolute addresses, so the compacted layout depends on where the code and the thunks landed, and a `b` or `bl` to a thunk out of range goes through a jump island allocated during the copy. On x86_64 nothing moves: every jump already has a 32-bit displacement.

2. Side tables. `JIT::link` fills the switch tables with code addresses, links the far calls and fills the `doneLocation` of each mold and call record.

3. Bytecode map. The positions in `m_labels` become `m_jitCodeMap`, bytecode index to code address. It holds instruction starts only; the labels of the checkpoints inside multi-step opcodes die with the `JIT`. Installation uses the map to point the exception handlers at their `catch`; the LLInt's loop OSR entry and the OSR exits that land in baseline code, including those that resume after a checkpoint, use it to enter the function in the middle.

4. LinkBuffer finalization. `FINALIZE_BASELINE_CODE` runs the link tasks the emitters registered (on x86_64 the calls and jumps to thunks; on both, the Math IC location records) and flushes the instruction cache. `JIT::link` then builds the `BaselineJITCode`.

The `LinkBuffer` and the `JIT` object then die with everything the artifact did not take: labels, generators, far-call records, thunk link records and link tasks.

# The artifact

Through `DirectJITCode`, `BaselineJITCode` inherits the code's executable memory (`JITCodeWithCodeRef::m_executableMemory`), its normal entry (`JITCode::m_addressForCall`) and its arity-check entry (`DirectJITCode::m_withArityCheck`); through `MathICHolder`, the Math ICs, kept in four `Bag`s with no accessor. Only the pointers baked into the code reach a Math IC, and nothing maps one back to its bytecode. With `USE(BUN_JSC_ADDITIONS)` the class declares eleven fields:

| field | contents | written by |
|---|---|---|
| `m_unlinkedCalls` | one record per call site except `call_direct_eval`: bytecode index and `doneLocation`, sorted by bytecode index. OSR exit from the DFG and FTL searches it for the return address of an inlined call (`getCallLinkDoneLocationForBytecodeIndex`) | `JIT::link` |
| `m_unlinkedPropertyInlineCaches` | the IC molds, in emission order | `JIT::link` |
| `m_switchJumpTables` | code addresses for each numeric or character switch: one per value of a dense range, plus the default | `JIT::link` |
| `m_stringSwitchJumpTables` | code addresses for each string switch, read by `operationSwitchStringWithUnknownKeyType` | `JIT::link` |
| `m_jitCodeMap` | bytecode index to code address, searched by binary search | `JIT::link` |
| `m_constantPool` | the (table, index) pair of each slot, which installation resolves to the `CodeBlock`'s `FunctionExecutable` | `JIT::link` |
| `m_pcToCodeOriginMap` | code address to bytecode origin, delta-compressed, read by the sampling profiler and `CodeBlock::findPC`; null unless the `VM` builds these mappings, which the sampling profiler, an attached debugger or `alwaysGeneratePCToCodeOriginMap` turn on | `JIT::link` |
| `m_ownerWentAwayAt` | the heap's `lastGCBoundaryTime()` when the latest `CodeBlock` using the artifact was destroyed; `Heap::releaseUnusedSharedBaselineCode` compares it to decide whether to drop the copy parked in the `UnlinkedCodeBlock` | `CodeBlock::~CodeBlock` |
| `m_livenessRate`, `m_fullnessRate` | the share of value profiles holding a prediction, without and with the argument profiles respectively, measured when the baseline-to-DFG decision defers | `CodeBlock::shouldOptimizeNowFromBaseline` |
| `m_isShareable` | whether other `CodeBlock`s of the body may reuse the code | `JIT::link` |

Three parts of the artifact change after linking, for every `CodeBlock` sharing it: the Math IC regions of the code, with their snippets; `m_livenessRate` and `m_fullnessRate`; and `m_ownerWentAwayAt`.

# Handoff

Finalization runs on a thread that owns the `VM`, never on a worklist thread: when a slow path finalizes the `VM`'s ready plans, or when `Heap::completeAllJITPlans` waits for all of them before deleting code or walking every `CodeBlock` ([install.md](install.md) lists the doors). A compilation fails only when it cannot get executable memory. Plans compile with `JITCompilationCanFail`: `JIT::link` returns null, and finalization sets the `CodeBlock`'s `m_didFailJITCompilation` and calls `dontJITAnytimeSoon`, which defers the body's shared counter indefinitely and so stops the LLInt tier-up of all its `CodeBlock`s. Under `JITCompilationMustSucceed` the allocator crashes the process instead.

On success, `JIT::finalizeOnMainThread` adds the code's size per bytecode word to `VM::machineCodeBytesPerBytecodeWordForBaselineJIT`, from which the tier-up threshold estimates the next compilation's executable memory, and calls `CodeBlock::setupWithUnlinkedBaselineCode`; the plan then calls `installCode` and `jitSoon`. What installation reads and builds from the artifact is in [install.md](install.md).

# What the code embeds

Once linked, the code refers to three kinds of things: positions in its own allocation, which move with it; per-`CodeBlock` state, reached through the base registers; and objects outside both, whose addresses or identities are fixed in the bytes. This section lists the third kind by owner. Apart from each Math IC's slow-call site, no record of where these references sit survives linking.

| owner | what the code holds |
|---|---|
| the artifact | the storage address of each dense `switch_imm` and `switch_char` table; each Math IC's address, passed to its slow operation when that operation can regenerate the IC (otherwise the profile's address goes instead); after regeneration, the jumps between the code and the Math IC snippets |
| the `UnlinkedCodeBlock` | the address of every arithmetic profile the code writes or passes to a profiled operation; the pointer of each cell constant it owns, as an immediate (`emitGetVirtualRegister`, `op_mov`) or an operation argument (`new_reg_exp`'s `RegExp`); the `StringImpl*` of constant atoms compared by `stricteq`, `nstricteq`, `jstricteq` and `jnstricteq`, of the case atoms of an inline `switch_string`, and of the identifiers passed by `put_getter_by_id`, `put_setter_by_id` and `put_getter_setter_by_id` |
| the `VM`: itself and its fields | the `VM*` as an operation argument (write barriers, the slow path of `new_object`, `catch`, `debug`, `loop_hint`'s `operationOptimize`) and as the base through which `catch` reads `callFrameForCatch`; the addresses of the soft stack limit (entry), the trap bits (`check_traps`), `topEntryFrame` (`catch`, `loop_hint`'s slow path), `targetMachinePCForThrow` (`catch`), the heap's barrier threshold (each write barrier), the pending exception (after an operation returning a register pair, such as `iterator_next`'s `operationIteratorNextTryFast`; the others return the exception in a register), the taint flag (`op_enter`, under `couldBeTainted()`) and, on ARM64, the mutator fence flag (`new_object`, `create_this`). `op_enter` reads the trap bits and the barrier threshold through the `VM*` stored in the `CodeBlock` instead. Debug builds add `vm.topCallFrame` and the `vm.exception` compare |
| the `VM`: objects it owns | the empty string (`iterator_next`), the small-strings sentinel (`enumerator_next`), `syncResumeCallCache` (`async_iterator_next`); in the profiler opcodes, the type profiler's log, the `TypeLocation`s and the control-flow counters; in the debugger's ShadowChicken opcodes, the log's cursor and end; under the options above, the per-bytecode profiler counters, the exception-fuzzing buffer and the loop-hint counters |
| the `VM`: thunks | `HandleException` (target of every exception check), `ThrowStackOverflowAtPrologue`, the virtual call thunk (`call_direct_eval`'s slow path), the handlers of `op_enter`, `check_traps` and `throw`, `valueIsTruthy` and `valueIsFalsey` (`jtrue`, `jfalse`), the `resolve_scope`, `get_from_scope` and `put_to_scope` thunks, the IC slow-path thunks, one thunk per `slow_path_*` function and, in debug builds, the consistency-check thunk |
| the process | the C++ operations, each a far call (the ShadowChicken and exception-fuzz helpers call theirs through a pointer immediate instead); the process-wide thunks `ReturnFromBaseline`, `LLInt::defaultCall()` and `LLInt::arityFixup()`; `structureIDBase`, ORed in as a 64-bit immediate wherever the code decodes a `StructureID` (`typeof_is_undefined`, `has_structure_with_flags`, `jeq_null`, `jneq_null`, `eq_null`, `neq_null`, `get_prototype_of`, `get_property_enumerator`, `enumerator_put_by_val`); `g_superSamplerCount` (`super_sampler_begin`, `super_sampler_end`); the probe callbacks of `traceBaselineJITExecution` |
| the build | the layout constants baked as displacements: `BaselineJITData` and `HandlerPropertyInlineCache` sizes and offsets, `CodeBlock`, `VM`, cell, object and `Structure` field offsets, and the metadata sizes and alignments generated from the bytecode list |

The `VM`-owned references make the code specific to one `VM`. `HandleException`, `ThrowStackOverflowAtPrologue` and the virtual call thunks exist from the `VM`'s construction; the other per-`VM` thunks are generated on first request, possibly by the compiling thread (`JITThunks::ctiStub`).

On x86_64, a nonzero pointer immediate or absolute address is a 10-byte `movabs` into a register; an absolute load or store through `rax` or `eax` instead embeds the 8-byte address in a single `mov`. On ARM64 the length depends on the value: `move` picks the number of `movz`, `movn` and `movk` instructions, and an address kept in a cached temp register can reuse the previous value there, emitting only the halves that differ, a load offset or nothing. `structureIDBase` is the high half of the structure reservation's address, a multiple of 4 GiB: on x86_64 a nonzero base always takes a `movabs` into the scratch register plus an `or`, while ARM64 emits one `orr` when the base is a valid logical immediate and otherwise a `move` of value-dependent length into the data temp register plus a register `orr`. Far calls use a fixed-width move into a register plus an indirect call on both architectures. Near calls and jumps to thunks are PC-relative (`call rel32` and `jmp rel32`, `bl` and `b`), and conditional branches to `HandleException` are `jCC rel32` on x86_64 and one or two instructions on ARM64, depending on the distance the compaction measured. `LLInt::defaultCall()` is the exception: the call fast path loads it as a pointer immediate.

Emission is also nondeterministic beyond the entry's `nop`. On x86_64 the assembler blinds some untrusted immediates at random, emitting a transformed value that the code undoes at run time: a 32-bit immediate from 2^24 up or below -256 once in 64 emissions, a pointer immediate once in 64, and a 64-bit cell pointer about once in 4096, since it is screened as a double first and needs two draws. References go in as trusted immediates, which are never blinded, except cell constants, loaded as `Imm64` values, and the atom keys of an inline `switch_string`, compared as `ImmPtr`. ARM64 never blinds. `BinarySwitch` also shuffles its tests with a generator seeded from a counter the process increments per switch. The assembler's generator takes one cryptographic draw per process and then a counter per assembler, and no option fixes it, so two compilations of the same body produce different bytes, and the sites of external references cannot be found reliably in the bytes afterward.

# Sharing between CodeBlocks

The engine reuses an artifact for the other `CodeBlock`s of the same `UnlinkedCodeBlock` in the same `VM` when `useBaselineJITCodeSharing` is on and `m_isShareable` is still set, which only the profiler opcodes clear (the parking slot and the doors are in [install.md](install.md)). Those `CodeBlock`s share every owner of the previous section, so its references are the same for all of them. What can differ is the per-`CodeBlock` data the compilation turned into code:

| `CodeBlock` data that went into the compilation | what comes out in the code | in another `CodeBlock` |
|---|---|---|
| `m_modeMetadata`, in `get_by_id`, `iterator_open` and `async_iterator_open` | own or prototype access, guarded against the structure in the live cache | correct; diverging costs a slow path |
| `m_enumeratorMetadata`, in `enumerator_next` | own structure fast path, guarded on the enumerator's flags | correct; diverging costs a slow path |
| a global `ResolveType`, in `resolve_scope`, `get_from_scope` and `put_to_scope` | a compare against the type read at compile time, before using the member | correct; diverging costs a slow path |
| `ModuleVar`, in `resolve_scope` | a `loadPtr` of the environment at a fixed metadata offset, with no type check | the member at that offset is whatever that `CodeBlock`'s link stored for its own resolve type |
| `ClosureVar`, in `resolve_scope` | below depth eight, one `loadPtr` per scope link; from eight up, a loop over the depth read from metadata that assumes it is nonzero; no type check either way | below eight its depth is ignored; at any depth its resolve type is |
| `ClosureVarWithVarInjectionChecks`, in `resolve_scope` | a call to a thunk that checks the var injection watchpoint and walks the depth read from metadata, with no type check | its resolve type is ignored |
| `ClosureVar` in `get_from_scope`; `ClosureVar`, `ResolvedClosureVar` and `ClosureVarWithVarInjectionChecks` in `put_to_scope` | a load or store at the metadata operand's index in the scope, with no type check | its resolve type is ignored |
| `ModuleVar`, in `put_to_scope` | a dispatch on the type whose every slow case throws the read-only error a write to an import gets (`slow_path_throw_strict_mode_readonly_property_write_error`) | any other kind throws that error as soon as it takes a slow case |
| `capabilityLevel()` | whether the code has the profile writes, the entry's argument profiling, the tier-up increments and `loop_hint`'s `operationOptimize` call | only `operationOptimize` rechecks it |
| `couldBeTainted()` | a `store8` to the `VM`'s taint flag, emitted by `op_enter` | nothing rechecks it |

Runtime promotions only produce global kinds: an `UnresolvedProperty` site becomes `GlobalProperty`, or `GlobalLexicalVar` once a global lexical binding appears (`slow_path_resolve_scope`, `operationResolveScopeForBaseline`, `tryCacheGetFromScopeGlobal`, `tryCachePutToScopeGlobal`), which is why the global row is guarded. The closure and module kinds come from the nesting of the text, are fixed at link time and are the same in every link of that body; the emitters say so in comments (`JIT::emit_op_resolve_scope`, `JIT::emit_op_put_to_scope`, `JIT::emitSlow_op_put_to_scope`) and bake them. `ClosureVarWithVarInjectionChecks` in `get_from_scope` is guarded by accident: its thunk selection mixes `if` and `else if`, so the generic thunk overwrites the choice, and since that thunk only handles global kinds, the site calls the C++ slow path on every execution. The `CodeBlock`s of one body in one `VM` agree on `capabilityLevel()` unless their executables differ in `setNeverOptimize`.
