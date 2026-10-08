# Abstract

JSC tiers up each `CodeBlock` (one function body, or the module level, which works like a main function) on its own, so in the same `.js` one function can be in the FTL and another in the baseline. A JIT pool thread normally compiles the `CodeBlock` while execution continues in the interpreter or the previous tier; with `useConcurrentJIT` off, compilation runs synchronously on the thread that requested it. On Linux, the pool uses `max(min(cores, 3), 2)` threads by default; on Darwin/ARM64, the minimum is 1 and the maximum is `min(4, cores)` (`Options::initialize`). Installation stops JS execution and runs on the thread that was executing it.

There are 2 main structures. The `UnlinkedCodeBlock`[^unlinked] carries everything that does not depend on the globalObject (the realm, the object that holds that context's globals and intrinsics): the bytecode, the literal constants, the names, the layout of the metadata table and what has been learned about types during execution. The `CodeBlock`[^codeblock] points to it and carries everything tied to a realm: where each global name ended up, the observed object shapes and the call targets.

The `BaselineJITCode` carries the machine code and the tables that locate a point inside it, one per compilation. If the `UnlinkedCodeBlock`'s slot is empty, `useBaselineJITCodeSharing` is on and the body has neither `op_profile_type` nor `op_profile_control_flow` (two Web Inspector profiling opcodes), installation hangs it in that slot, where it serves the body's other `CodeBlock`s. Each `CodeBlock` also holds the `BaselineJITData` that installation built for it, and the metadata table. The emitted code reaches both through `jitDataRegister` and `metadataTableRegister`, which the prologue loads from the frame's `CodeBlock`.

LLInt and Baseline collect two kinds of feedback on specific bytecodes. **Profiles** record what went through so a future tier can speculate; the iteration mode and array allocation ones are also read at runtime by the slow path that wrote them, and the arithmetic one by the Math IC when it regenerates the fast path. **Inline caches** hold an object's structure and the property's position, or a call's target, replacing a lookup with a test and an inline access; they speed up the current tier and feed the next ones. Only LLInt and Baseline collect profiles (they share the same structures). DFG and FTL accumulate their own inline caches and OSR exits, and an exit writes into those profiles, never into a cache.

# UCB generation

The `CodeBlock` is created by `ScriptExecutable::newCodeBlockFor` from an `UnlinkedCodeBlock`, which may already exist, come from the bytecode cache or need to be generated from source. During generation, each nested function becomes an `UnlinkedFunctionExecutable` kept in the unlinked one: name, source range and mode, with no AST and no bytecode. On the normal lazy path, when the LLInt starts running there is only one pair, the top-level one; the rest are born on the first call of that function in that mode.

Once the bytecode exists, the `CodeBlock` is built in two stages. The unlinked one only describes the metadata layout, so `UnlinkedMetadataTable::link()` allocates a zeroed buffer for the table during linking and fills in the offsets; destroying the table frees that buffer and leaves in the unlinked one only what is needed to rebuild the layout. Then `finishCreation` builds the rest: the constants become values of that realm, with the `SymbolTable` cloned; each nested function gets its own `FunctionExecutable` (in a module declaration it reuses the executable created at instantiation); the exception handler table is copied with each `nativeCode` pointing to the LLInt's `op_catch` dispatcher; and a pass over the bytecode initializes each call's `CallLinkInfo` and the profiles. The template object, the array of strings of a template literal, is left for the end because it can throw, and an exception in the middle of linking would break the CB.

Bytecode caching saves the bytecode and the unlinked data needed to rebuild it, including the executables of the nested functions, but not the warm-up: the profile vectors are recreated with no observations and the LLInt's tier-up counter is reset.

# Baseline

In the baseline, each bytecode that leaves one or more jumps in `m_slowCases` forms a diamond: fast path, slow path and meeting point. Opcodes with no slow case, such as `mov`, `jmp`, `nop` and `ret`, stay only in the main pass. Physically the two sides sit far from each other: JSC first emits all the fast paths in bytecode order and then all the slow paths together in one block:

```
normal entry    ->  prologue
                    fast path of bytecode 0, 1, 2, …        (join points here)
                    all the slow paths, in one block        (each returns to its join point)
arity check     ->  argument checker                        (fixes the arity and jumps to the normal entry)
entry
                    stack overflow exit                     (always emitted; jumps to the `ThrowStackOverflowAtPrologue` thunk,
                                                            running a second prologue first only when the arity checker found the overflow)
```

The entry with argument checking sits at a **higher** address than the normal entry, and it only exists when the `CodeBlock` belongs to a function and has at least one declared parameter; otherwise the two entries are the same address.

## Example

JSC's basic pipeline is parser → AST → BytecodeGenerator → bytecode (sort of a portable IR). For `function f(a, b) { return a + b; }` it will be something like:

```
[ 0] enter
[ 1] add  dst:loc5, lhs:arg1, rhs:arg2    ; add arg1 to arg2 and put the result in loc5
[ 7] ret  value:loc5
```

if necessary check [bytecodeoptcodes.md](./bytecodeoptcodes.md) for all opcodes

In theory nothing prevents compiling a baseline right at call 0, but it would make startup far slower and use much more memory. The LLInt has hardcoded fast paths; the baseline emits the same shape for almost every opcode and consults what was observed only where the shape changes.

For an `add`, llint (already loaded) does:

```
mov 0x0(%rbp,%rdx,8), %rax
cmp %r14, %rax / jb <double>   ; int32?
cmp %r14, %rsi / jb <double>   ; int32?
add %esi, %eax                 ; ← hardcoded fast path
jo  <slow>                     ; 
or  %r14, %rax                 ; restores the int32 tag (to add the i32 that sits in a nan-boxed JSValue
                               ;                         you have to strip the tag off the front)
movq %rax, 0x0(%rbp,<dst>,8)   ; writes
...                            ; notes in the arithmetic profile that an int32 went through
loadb / jmp opcodeMap[op]      ; next bytecode
```

The baseline, depending on the profile, generates:
```
                         (same as llint)          (int+int, int+double, double+int and double+double, all inline)
── EMPTY profile         ── INT32 profile         ── DOUBLE profile                  ── STRING profile
jmp <slow>               cmp %r14, %rsi           cmp %r14, %rsi                     movq 0x8(%r13), %rdi
                         jb  <slow>               jb  <double>                       movl $0x1, 0x24(%rbp)
                         cmp %r14, %rdx           cmp %r14, %rdx                     mov $<operationValueAdd>, %r11
                         jb  <slow>               jb  <double>                       call %r11
                         mov %esi, %eax           ... (same int32 pair as INT32)     test %rdx, %rdx
                         add %edx, %eax           <double>: vcvtsi2sd %edx, %xmm1    jnz <exception>
                         jo  <slow>               vaddsd %xmm1, %xmm0, %xmm0         movq %rax, -0x30(%rbp)
                         or  %r14, %rax           vmovq %xmm0, %rax
                         movq %rax, -0x30(%rbp)   ...
3 instructions           11                       32                                 9
```

PS: `add` uses an arithmetic cache, which consults the profile and can rewrite itself when it falls into the repatchable slow path (`JITMathIC::generateOutOfLine`). With an empty profile, that path is taken on the first execution; hitting an already emitted fast path rewrites nothing.

The trick is that the sequence stays inline instead of reading bytecode and doing `llint_op_[byte index]`, the main reason the baseline is ~2x faster than the LLInt.[^why]

The DFG goes much further than that. For `for (let i = 0; i < N; i++) sum += i` the Baseline pays:

```
loop:                      ; (the back edge lands on check_traps, 3 instructions before here)
  movq -0x30(%rbp), %rsi   ; sum, boxed
  movq -0x38(%rbp), %rdx   ; i, boxed
  cmp  %r14, %rsi
  jb   <slow>              ; is sum int32?
  cmp  %r14, %rdx
  jb   <slow>              ; is i int32?
  mov  %esi, %eax
  add  %edx, %eax
  jo   <slow>
  or   %r14, %rax          ; re-boxes
  movq %rax, -0x30(%rbp)
  …                        ; the i++ pays the same tests, and the i < N again
```

And the DFG, having speculated that both are int32:

```
loop:
  movl -0x30(%rbp), %eax   ; sum, raw int32 in the same slot
  movl -0x38(%rbp), %esi   ; i
  add  %esi, %eax
  jo   <osr exit>
  movl %eax, -0x30(%rbp)
  inc  %esi
  jo   <osr exit>
  movl %esi, -0x38(%rbp)
  movl 0x30(%rbp), %eax    ; N
  cmp  %eax, %esi
  jl   loop
```

OSR, On-Stack Replacement, swaps the code of a call frame already on the stack, without waiting for the function to return: an exit leaves code that failed a speculation for the baseline at the equivalent point, and an entry moves a running frame up. Normally you can only enter at the start of the CB, because the DFG expects the state in a different form and builds it there. Each loop iteration adds to the counter, and reaching the threshold inside a loop can schedule a DFG plan that asks for an additional entry into it. That entry reads the baseline frame's values from an auxiliary buffer and places them, already untagged, into the virtual registers the DFG code expects there. When the plan is ready, the compiling thread signals by zeroing the baseline counter; back in the handler, the JS thread finalizes the ready plans, installs the code and tries to enter through OSR. `DFG::prepareOSREntry` can still refuse the entry, for example if the values do not match what was expected, and execution then continues in the baseline.

That second entry lives in a table of the DFG code, indexed by bytecode index, and survives its first use: any invocation still in the baseline at that point can try to enter through it. After installation, new calls enter at the start of the installed code.

# How UCBs and CBs relate

What depends on the realm stays in the `CodeBlock`: the cloned `SymbolTable`, the template object, the `StructureID`s of the observed shapes and the resolution of each free variable, which `JSScope::abstractResolve` computes during linking and writes into the metadata. Shapes live in a single per-process reservation, shared by all VMs and recorded in `g_jscConfig`. On 64 bits, the `StructureID` holds the low 32 bits of the address; `decode()` clears bit 0 (`nuke`) and adds `structureIDBase`, which may differ from the start of the reservation when the reservation is smaller than 4 GiB.

Every `JSC::VM` has a `CodeCache`, a hashmap of `Strong` entries (GC roots) fed by four sources: the top level of a program, the top level of a module and an indirect eval, each as a UCB, and `new Function`, whose entry is a UFE with the UCB hanging from it. With `useCodeCache` on, a program or module UCB decoded from the provider's cache is also inserted into the map (`findCacheAndUpdateAge`). Nested functions stay in the UCBs. The same text under different flags generates different bytecode, so the key combines the source hash with seven flags: code type; the strict bit; the `JSParserScriptMode`, whether it is a module or not; derived context; eval context; arrow function context; and code generation mode. The flags do not cover every generation input: Bun's `evalMode` option changes bytecode without entering the key ([bun patches](knowledge/common/bunpatches.md#options-set-by-the-host)). The comparison also checks the length; the flags; the name and the end position of the parameters, which separate `new Function` entries; and the origin's host with `sourceOrigin().url().host()`. Under `USE(BUN_JSC_ADDITIONS)`, `SourceCodeKey::operator==` stops at these tests, without comparing the source text.

Two CBs can share a UCB when they hit the same key, across realms too when their executables end up pointing to the same UCB. Each program or indirect eval evaluation builds a new executable and looks up the UCB in the `CodeCache`. `new Function` goes further: the global object keeps the last eligible executable in a single-entry weak slot, and while it is alive and the source and mode criteria match, consecutive calls reuse the executable and its CB. Different text in between, the GC or a different origin can each make it miss (`tryGetCachedFunctionExecutableForFunctionConstructor`).

Each CB has its own tier (one may not even exist yet while another is already in the FTL), and the ladder is not mandatory: a `CodeBlock` created after the artifact is hanging in the unlinked one is born in baseline, without ever being interpreted.

Identical bodies in different scopes, `function f(){}; function g(){ function f(){} }`, and the same function used as `f()` and `new f()` generate different `UnlinkedCodeBlock`s. In the first case, each function has its own UFE and its own bytecode slots, and `unlinkedCodeBlockFor` consults those slots without searching the `CodeCache` for an identical body. In the second, the bytecode is different: `generateUnlinkedFunctionCodeBlock` receives the `CodeSpecializationKind` and passes it on to the `ExecutableInfo` as the constructor bit, which the `BytecodeGenerator` consults in about ten places: in that mode it turns off tail call optimization, changes the prologue and emits the class field initializer.

# Executable memory

Machine code lives in a reservation created once per process, 512 MB on ARM64 and 1 GiB on x86_64. It is reserved whole because a jump is an address embedded in the instruction: if a second mmap landed GBs ahead, the jump would have to become a `movz`/`movk` plus a jump through a register, which would push everything below it down and cascade into every displacement already written. On x86 a relative `call` reaches ±2 GB and covers the whole pool; on ARM a `bl` only reaches ±128 MB, so to reach further within the pool ARM plants an island midway, a 4-byte jump that does nothing else.

The pool is sliced into 112 MB regions whose last 16 MB are an island band; on ARM four of them fit in the reservation, and the fifth goes in truncated, with 64 MB of code and no band. A jump that crosses regions becomes a chain (code, island, island, code) in which each link is within range. If a region's band runs out and one more island is needed, the process dies.

Whoever asks for executable memory passes a `JITCompilationEffort`, `CanFail` or `MustSucceed`. With `CanFail`, `ExecutableAllocator::allocate` returns null without touching the allocator if what is already allocated plus the request exceeds `(pool − islands) × 0.75`. With `MustSucceed` it skips that check and calls `CRASH()` if the allocation fails.

Only what is essential to run uses `MustSucceed`: the thunks, the birth of a baseline `CodeBlock` with the LLInt off, and the FTL's lazy slow path. Compilations that have somewhere to fall back to, from the baseline onward, plus an inline cache's stub and a Math IC's regeneration, fail silently when they cannot copy the finished code into executable memory: the cache gives up and stays on the slow path, the Math IC points the call at a slow path that cannot be repatched and never regenerates again, and the tier-up does not happen.

# Tier-up

Each tier-up works with points (500 to leave the LLInt, 1000 for the DFG, 64000 for the FTL). A complete pass through a function is worth 15 and each loop iteration (for/for-in/for-of/while/do-while) is worth 1. In the LLInt the 15 are split between prologue and epilogue, 5 and 10; in the baseline they go in whole at the entry, where it already materializes the base registers from the `CodeBlock`; and in the DFG they go at the return, because an OSR exit at the start or in the middle would count a pass that did not finish.

When the counter reaches zero or more, execution falls into the tier-up handler, which compares the executions counted so far with a corrected threshold: the base multiplied by `available / (available − allocated − estimate)`, then reduced by `min(initial threshold, slice ceiling) / 2`. The estimate is `(mean + standard deviation) × cost`, where the mean is the pool bytes per bytecode word that the `VM` accumulates on each baseline compilation. Available here is the reservation minus the island bands and minus a 25% margin. The ratio only applies while `allocated + estimate < available`; otherwise `ExecutableAllocator::memoryPressureMultiplier` returns 1.0.

The armed value is capped at a slice ceiling, 1000 in the baseline and 30000 in the upper tiers; when a capped slice runs out, the handler recomputes the threshold against the occupancy at that instant and arms the next one. When the `CodeBlock`'s bytecode cost reaches ten thousand (the cost is summed instruction by instruction, each worth the size of its opcode plus one), the baseline ceiling is multiplied by the square root of the size factor, `0.826 + 0.0615·√(cost + 1.024)`, because each stop also drains profiles, which is expensive to repeat for a giant function.

From baseline→DFG upward, the armed threshold is multiplied by the same size factor, whatever the cost, and by two raised to the number of reoptimizations already attempted (capped at 21). The product is clipped to `INT32_MAX` (`clipThreshold` in `CodeBlock::adjustedCounterValue`), and `ExecutionCounter::setThreshold` treats an active threshold of `INT32_MAX` as `deferIndefinitely`. At the cap, a base of 1000 passes that limit once the size factor exceeds about 1.024, which a bytecode cost of 10 already gives (the quick-DFG factor needs a far larger body); from then on arming that `CodeBlock` through `setThreshold` defers it indefinitely. An OSR exit writes a slice directly through `handleExitCounts`, so the baseline counts one more slice before `checkIfThresholdCrossedAndSet` reaches `setThreshold` and defers it. A new `CodeBlock` of the body starts its reoptimization count at zero and can tier up again.

Before approving a baseline→DFG compilation, the handler drains the value profiles and measures two coverages, the fraction of profiles whose prediction is not `SpecNone`: among non-argument profiles it must reach 0.75, and among all profiles, arguments included, 0.35. Both count accumulated predictions, not the current occupancy of the buckets (`shouldOptimizeNowFromBaseline`); the point is to avoid speculating on an empty profile and taking an OSR exit on the first execution. In the Mac quick tier-up, both requirements are multiplied by 0.85. A category with no profiles counts as 1.0. Bodies with a bytecode cost of at least 5000 also pass when both rates are nonzero and equal to those of the previous attempt, because waiting longer may teach nothing. Otherwise the handler defers: it rearms the full threshold and returns no, so the body counts again before being reevaluated. Once it reaches five deferrals, it drops the coverage requirement and allows the compilation. These numbers are the `Options` defaults.

`ExecutionCounter` has three fields: `m_counter` is the current point counter; `m_totalCount` adds the points already earned plus the pending threshold, so the executions counted so far are always `m_totalCount + m_counter`; and `m_activeThreshold` is the base threshold that the correction multiplies at each stop. The three tier-ups use this counter in different places: LLInt→baseline's lives in the `UnlinkedCodeBlock` (every CB of that body adds to the same one); baseline→DFG's in the `BaselineJITData`, one per `CodeBlock`; DFG→FTL's in the `DFG::JITData`, one per DFG CB, but with the threshold computed from the baseline CB.

In LLInt→baseline the armed threshold is the base × 1 if the engine still knows nothing about that UCB, × 4 if one of its `CodeBlock`s died before any of them entered the DFG through a successful OSR entry (the destructor does not check whether that CB reached the DFG), and ÷ 2 after one of them enters that way, which prevails from then on. Having compiled is not enough, nor is calling the DFG code through the function's normal entry; only an OSR entry counts, into a loop or at the first instruction, which `op_enter` triggers when the counter crosses the threshold.

## Jettison

Jettison is discarding code; out of an optimized tier, execution normally goes back to the baseline. There are seven causes: too many OSR exits, a watchpoint firing, a failed OSR entry, a weak reference dying, age, the debugger and VM traps. Each speculation exit counts against the `CodeBlock` it left (exception checks and unwinding do not count), which is jettisoned past 100 exits, or past 5 when that body's baseline has already asked to tier up again or some inlined function in the chain has already tried to enter a loop through OSR. Both numbers double with each reoptimization already attempted.

The lower number targets loops because there the reset is expensive: the body keeps spinning in baseline until it warms up a whole threshold again. Outside a loop an exit gives back one invocation; inside one, it gives back the rest of the loop. With 5 instead of 100, the engine favors recompiling right away, with what the exit taught, over paying twenty times that waste.

Below the exit threshold nothing is thrown away, but the exit touches the body's counter: the exit stub directly arms a slice of the adjusted long-warm-up threshold, otherwise the baseline would immediately order a recompile of the code that just failed. If the counter is already asking to tier up, the exit instead calls `operationTriggerReoptimizationNow`, which rechecks the exit criteria and, without enough exits, only rearms the counter with `optimizeAfterLongWarmUp()`. When code is jettisoned for any reason other than age or traps, the bytecode points that have already exited are written to the `UnlinkedCodeBlock`'s exit profile, which outlives the `CodeBlock`; that is how the next compilation knows where to speculate less.

Speculation failures (too many exits, watchpoint, failed OSR entry) raise the future threshold: they increment the reoptimization counter and clear that tier's quick tier-up bit in the `UnlinkedCodeBlock`, which records that the body has already proven worth the tier-up. While set, the DFG's bit multiplies the threshold by 0.2 outside the Mac and by 0.15 on the Mac; the FTL's only lowers it on the Mac, by the same 0.15 (elsewhere the factor is 1 and the bit changes nothing). Installing a tier sets its bit, on every installation and platform, except that outside the Mac a cleared DFG bit never comes back. A failed compilation also clears its tier's bit.

Once the handler approves, the tiers differ in how they learn that the compilation is ready. LLInt→baseline enqueues without rearming: the counter stays at zero, so every following call falls into the handler again, and one of them finds the code ready. Baseline→DFG also leaves the counter untouched after enqueueing; it stays crossed and is rearmed on the next entry, with the thread still compiling. DFG→FTL rearms at enqueue time. From there the last two behave the same: execution counts up to the full threshold again, and if it gets there with the compilation still in progress, the handler rearms, checks the worklist and gets `Compiling`. When the plan is ready, the compiling thread zeroes the counter, but that write can race with the JS thread's rearm and does not guarantee the very next stop; on returning to the handler, the JS thread finalizes and installs the ready plans (`setOptimizationThresholdBasedOnCompilationResult`).

# Locks

Three kinds of thread look at the same `CodeBlock`: the ones that run JS, the compilation ones and the GC ones.

Only the threads that run JS can allocate `JSCell`s, create an `Identifier` and mutate a `Structure`. They do it under the `JSLock`, one per `JSC::VM` (`VM::m_apiLock`), which also guarantees that only one thread runs JS in that VM. `JSLock::lock` acquires it; then `didAcquireLock` obtains heap access, if needed, and registers the thread with the collector when the `uid` changes.

Compilation threads never allocate a `JSCell`, and the constructor asserts this with `ASSERT(!isCompilationThread())`. But they read and mutate the heap: `CodeBlock::capabilityLevel()` memoizes by writing `m_capabilityLevelState`, and `JITPlan::iterateCodeBlocksForGC` generalizes: "Compilation writes lots of values to a CodeBlock without performing an explicit barrier. So, we need to be pessimistic and assume that all our CodeBlocks must be visited during GC".

Marking follows pointers to everything reachable from the JS threads' stacks and from the other roots that the `JSC::VM` and the heap visit (`Heap::addCoreConstraints` and `gatherVMRoots`). They include the `Strong`s, handles that C++ declares as roots; the values the host protected with `JSValueProtect`; the exception being propagated, kept in a `VM` field while the `throw` climbs; the argument lists in transit; the small strings; and the `CodeBlock`s with a compilation in progress. On the heap, marking works with JS running because the `WriteBarrier`s notify the GC when references change. Doing the same for stacks would be too expensive, so the collector stops the world to mark: the JS threads because of their stacks, the compilation threads because of their barrier-free writes into the `CodeBlock`.

To stop the compilation threads, the GC takes the `m_rightToRun` that each `JITWorklistThread` holds while it works. To stop the JS thread, `Heap::stopTheMutator` looks at the `hasAccessBit` in `m_worldState`: without heap access, the collector can mark the world as stopped and drive the collection; if the thread still holds it, the collector sets the `mutatorHasConnBit` and schedules a timer on the run loop. The thread cooperates through `stopIfNecessary`, in the allocation slow paths and in the timer, and takes over driving the phases. When it hands the driving back with `relinquishConn`, it clears that bit while still holding heap access. Ownership of heap access decides this handoff, not simply whether the thread is executing JS.

The mutator enters collector phases only through `stopIfNecessary` and similar explicit calls. Under GC deferral the allocation path neither stops nor requests a collection, so a thread that keeps heap access inside a deferral scope and makes no explicit stop call keeps every collection, including one already running, out of the stopped-world phases (Begin, Fixpoint, Reloop and End). Concurrent marking still proceeds, but finalization, jettisons by the GC, IC resets, stub deletion and the sweeps after them wait until the thread reaches a stop point or releases heap access. `DeferGCForAWhile` differs from `DeferGC` in never collecting at scope exit.

The DFG and the FTL yield between phases, so the stop the world waits for them; the baseline opens a single safepoint and compiles entirely inside it, so the collector comes in whenever it wants and finalization runs with that compilation alive.

# Heap

JSC has four main `mmap`ed heaps: the bmalloc one (which today is libpas), which holds the `MetadataTable`, `BaselineJIT Data/Code`, the IC handlers and the blocks where cells live; the `Structure`s one, of 4 GiB; the Gigacage, of 64 GiB, which holds typed arrays, isolating their pointer against the mask; and the executable memory pool.

# GC

The collector marks what is alive and kills by omission, in three acts per cycle: marking, which alternates stopped-world windows with running-world windows until a stop adds nothing more; finalizing, with the world stopped, which reacts to what was left unmarked; and sweeping, where the destructor runs and the memory comes back. Sweeping is normally incremental, with the world running, but it can also be synchronous depending on the collection policy (`Heap::shouldSweepSynchronously`).

There are two kinds of cycle, both starting from the same roots. A full one clears the marks and walks everything. An eden one does not descend into anything already marked by the last full or the edens since; it visits the old objects that a `WriteBarrier` noted as changed, to catch new objects they reference. The next full discards all those accumulated marks and sweeps everything. The previous collection picks the kind: the collector keeps a size ceiling for the heap, recomputed on each full in proportion to what survived it, and if the space left at the end of an eden falls below `minEdenToOldGenerationRatio` of that ceiling, the next one is a full. The default is one third of remaining space, equivalent to going past two thirds occupancy (`Heap::updateAllocationLimits`).

The `CodeBlock` is a `JSCell`, but it has its own rules for visiting and retention. In `MarkedBlock`s, cells live in 16 KiB blocks sliced into 16-byte atoms, and each block's header carries a bitmap with one bit per atom, set when the cell that starts there has been marked live. Not everything in the heap is a cell: there is `Auxiliary` storage for buffers, and large allocations can use `PreciseAllocation` instead of these blocks.

During marking, the `SlotVisitor`'s first visit to a `CodeBlock` in LLInt or baseline already drains the value profiles, even with the world running (`CodeBlock::visitChildren`). The `CodeBlock` finalizes itself: every visited code block registers in a set (another bitmap over the same blocks), and at the end the collector intersects that set with the marks and calls the finalizer of those still marked. In LLInt and baseline, that finalizer redoes the value profile drain and also covers the array and array allocation profiles, classifying the samples and emptying the buckets even when they are still alive; every inline cache whose structure died goes back to the empty state; and every call whose callee died is disconnected (`reconcileWeakReferencesAtGCEnd`). An unmarked one gets no finalizer call: if the owner executable is marked, its own finalizer, which runs earlier, jettisons the code block and cuts the edge.

# Lifecycle

## CB

An LLInt CB starts with a 5-second protection window; in the baseline the base window is 15. The protection comes from the owner executable's visit and is not a root by itself. In the Bun profile, with `useExecutionCountForCodeBlockAging` on, the window can be renewed when the counter shows activity (see the [fork's aging](knowledge/common/bunpatches.md#code-aging-pr-557-codeblockshouldjettisonduetooldage)). A CB that has lost the protection still survives if something else reaches it: a stack, an in-flight compilation plan, or an optimized CB that has it as its alternative. Otherwise it can be collected, by an eden collection if it is still young, and the destructor frees the metadata table and the `BaselineJITData`.

The `BaselineJITCode` can remain in the unlinked one's slot, which `prepareForExecutionImpl` checks when the next CB is created, before a single bytecode runs, and so does the LLInt's tier-up handler. The age `jettison` can clear that slot, and so can `Heap::releaseUnusedSharedBaselineCode` at the end of a full: with sharing and counter-based aging on, it frees code that only the UCB holds and whose last owner CB died more than 45 s ago under the defaults. `deleteAllCode` also empties the slot. Executable memory goes back to the pool when the last reference drops, not necessarily when a CB dies.

The DFG and the FTL have another way to survive: if all the weak references to the cells and structures they speculated on are alive, `determineLiveness` can keep them. In the fork, that path does not save an optimized one that has aged. If something else reaches the CB, the CB marks those dependencies itself (`stronglyVisitWeakReferences`); a dependency not marked through another path does not, by itself, imply the CB's death.

It's kind of a hunger games of CBs. If the baseline tiers up, the DFG `CodeBlock` points to it as its alternative and shields the fallback. Tiering up to the FTL shields only the baseline, not the DFG, because the FTL `CodeBlock`'s alternative is the baseline too. The shield drops when the optimized one ages with no other reference keeping it alive (`ScriptExecutable::visitCodeBlockEdge`).

An IC stub tracked by the GC is freed only once no reference remains and no stack is still executing it. If the last owner lets go first, the stub waits for the GC; if the GC has already removed it from the set because its owners died and it is not executing, the last `deref` can destroy it directly (`JITStubRoutineSet` and `GCAwareJITStubRoutine::observeZeroRefCountImpl`).

PS: an exit jumps to the LLInt instead of the baseline if the inlined callee was never compiled: the DFG inlines functions that only ran in the interpreter, and the source has a FIXME admitting it.

## UCB

UCBs form a tree, normally kept alive from a root down: a body's UCB keeps one `UnlinkedFunctionExecutable` per nested function, and each of those keeps the UCB of its own body once that UCB is generated. `CodeCache` entries can keep the top levels of program, module and indirect eval, and the UFE, not the UCB, of a `new Function`. Direct eval does not go through that cache: its executable sits in the rare data of the `CodeBlock` where the call is. Losing that edge does not guarantee death, because a live CB or a function that escaped can also keep the executable and the tree reachable.

The UFE→UCB edge can also be weak: with `useUnlinkedCodeBlockJettisoning` on (off by default) or in mini mode, UFEs that did not come from the cache can stop marking old UCBs whose `didOptimize()` is not `True`, and clear the slots if they end up unmarked (`codeBlockEdgeMayBeWeak` and `UnlinkedFunctionExecutable::visitChildrenImpl`). `VM::deleteAllCode` can also discard the UCBs explicitly, and the Bun host runs one in every long-lived `bun run` process right after the entry finishes ([bun patches](knowledge/common/bunpatches.md#conditions)).

The `CodeCache` regulates itself by reuse distance, how much text it has handled since an entry was last touched. On each successful lookup in the map it compares that distance with the current capacity, raising the capacity by `4 × 32 × length` when the distance exceeds it and lowering it by `4 × length` when the distance falls below half, respecting a floor. It can prune when it is above capacity or reaches 2000 entries; below 2000, it still waits ten seconds since the last prune, unless the stored contents have grown by at least 16 million code units in that interval (net growth of `m_size`, not all new text processed). It then updates the capacity floor from that growth and evicts `m_map.begin()` in a loop until it is within capacity and below 2000 entries (`CodeCacheMap::prune` and `pruneSlowCase`).

Before removing a UCB entry, it calls `commitCachedBytecode()`, which saves the bytecode to disk if the host implements it (the `SourceProvider` virtuals that would save it are empty, but the API is there); a UFE entry, from `new Function`, skips that step. Removing the entry destroys the `Strong`, not the UCB: a function that escaped, for example, keeps its UFE and `m_topLevelExecutable` (`FunctionExecutable::visitChildrenImpl`), and a registered module can stay pinned to the module loader. The next lookup for that body misses the in-memory cache and tries to decode or generate the bytecode; if it creates a new UCB while the old one is still alive, the two coexist. When the UCB dies, it drops the `RefPtr` to whatever code is still in the sharing slot.

# Structures

[^unlinked]:

```cpp
// bytecode/UnlinkedCodeBlock.h
std::unique_ptr<JSInstructionStream>     m_instructions;      // the bytecode
FixedVector<WriteBarrier<Unknown>>       m_constantRegisters; // literal constants
FixedVector<SourceCodeRepresentation>    m_constantsSourceCodeRepresentation;
FixedVector<Identifier>                  m_identifiers;
const Ref<UnlinkedMetadataTable>         m_metadata;          // layout
OutOfLineJumpTargets                     m_outOfLineJumpTargets; // hash; the jump that did not fit in the operand
FunctionExpressionVector                 m_functionDecls, m_functionExprs;
FixedVector<UnlinkedValueProfile>        m_valueProfiles;     // prediction, no buckets
FixedVector<UnlinkedArrayProfile>        m_arrayProfiles;
FixedVector<BinaryArithProfile>          m_binaryArithProfiles; // already classified
FixedVector<UnaryArithProfile>           m_unaryArithProfiles;
DFG::ExitProfile                         m_exitProfile;
BaselineExecutionCounter                 m_llintExecuteCounter; // the tier-up counter; every CodeBlock of the same body shares a single one
RefPtr<BaselineJITCode>                  m_unlinkedBaselineCode; // the sharing slot
VirtualRegister m_thisRegister, m_scopeRegister;
unsigned m_numVars : 31;
unsigned m_numCalleeLocals : 31;
unsigned m_numParameters : 31;

std::unique_ptr<RareData> m_rareData;   // exception handlers and jump tables,
```

[^codeblock]:

```cpp
// bytecode/CodeBlock.h
WriteBarrier<UnlinkedCodeBlock>  m_unlinkedCode;    // points to the one above
WriteBarrier<JSGlobalObject>     m_globalObject;    // the realm
WriteBarrier<ScriptExecutable>   m_ownerExecutable;
VM* const                        m_vm;
const void* const                m_instructionsRawPointer;
mutable ConcurrentJSLock         m_lock;            // protects the ICs: whoever modifies one, and whoever reads from another thread
RefPtr<JSC::JITCode>             m_jitCode;
void*                            m_jitData;         // BaselineJITData or DFG::JITData
RefPtr<MetadataTable>            m_metadata;        // the INSTANCE, with the profiles. neighbor of m_jitData
                                                    // by contract: the prologue loads both with one loadPairPtr
CompressedLazyValueProfileHolder m_lazyValueProfiles;
FixedVector<ArgumentValueProfile> m_argumentValueProfiles;
Vector<WriteBarrier<Unknown>>    m_constantRegisters;
FixedVector<WriteBarrier<FunctionExecutable>> m_functionDecls;
FixedVector<WriteBarrier<FunctionExecutable>> m_functionExprs;
StructureWatchpointMap           m_llintGetByIdWatchpointMap;
SentinelLinkedList<CallLinkInfoBase, BasicRawSentinelNode<CallLinkInfoBase>> m_incomingCalls; // who calls this CB, to relink on a tier change
#if ENABLE(JIT)
uint8_t m_capabilityLevelState : 2;                 // memoizes the gate that turns on profile emission
#endif
WriteBarrier<CodeBlock>          m_alternative;
```


# Notes

[^why]: The LLInt ends every handler with a `loadb` of the next opcode and `jmp opcodeMap[opcode]`, an indirect jump that is hard to predict. While instruction N executes, the CPU is already fetching and decoding the following ones; if the prediction misses, it discards that work and starts over at the target. The baseline removes one of these dispatches per bytecode. The [WebKit blog](https://webkit.org/blog/10308/speculation-in-javascriptcore/) calls that removal the main reason for the ~2× gain.

PS: jsc has four languages: C++, the LLInt's offlineasm (.asm, translated at build time), the .rb files that generate bytecode and metadata from BytecodeList.rb, and the assembly the emitters produce at runtime. be careful grepping only cpp, especially when talking about the llint
