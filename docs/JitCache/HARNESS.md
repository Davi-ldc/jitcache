# JITCache harness

This document says how JITCache is tested and measured, and what counts as passing. [THREAD](THREAD.md)'s Verification section is the contract, and this document cites it instead of restating it. The tests themselves live in the SPECs, each lane's in its own; [SPEC-integrator.harness.md](specs/SPEC-integrator.harness.md) holds the runner's mechanics, and the implementation tasks hold everything else.

## Principles

Tests and microbenchmarks of single cases are the core of the feedback loop. When a promise can be stated as a count or as an identity of code, a test checks that count or that identity, and no timing stands in for it: the skipped warm-up is a set of per-body event counts, and "Other VMs emit unchanged" is a comparison with the pinned engine's code. Time is measured only where no deterministic form exists: THREAD's installation bound and the bench's workloads. Benches are alerts from one checkpoint to the next, never targets. THREAD's priorities are relative, and pushing one number at any cost breaks its intentions.

## Criteria

| THREAD guarantee | observable | passes when | when it runs |
|---|---|---|---|
| A consumer run is observably identical to the JITCache-off run (Verification) | the oracle | output and heap description equal | every run of every test |
| Each restored piece equals its native twin or capture record; a lane lands when its pieces do (Verification, Execution) | the twin report | no difference; a skip only in a run where it is allowed; a heap coincidence that does not repeat | every test |
| An import runs without interpreting, compiling again or relearning (opening) | per-body event counts | the counts the case determines, exactly | every test that imports |
| Other VMs emit unchanged (Capture) | the pin comparison | no difference outside the two native fixes | the end of every task |
| Failures and their outcomes, failed strict checks (Failures, Session) | fault injection | the outcome THREAD gives, `status` naming the step, twins and oracle clean afterward | every test of a failing step |
| Commits are process-crash-consistent (Capture) | producer kills | the state harness section 12 gives at each point | every kill test |
| The same guarantees under concurrent schedules (Capture, Storage) | the concurrency family | no failure in `concurrencyRepetitions` repetitions | one repetition at the end of a task that touches those paths; all of them at each milestone |
| Importing a body costs at most a fixed fraction of compiling it (opening) | a microbenchmark per body | alerts only, when the ratio exceeds `installationBoundFraction` for a body of at least `installationBoundSmallestBody` | each milestone, in the release build |
| A consumer's first run matches the producer's warmed run; capture pause, memory and serialization (opening, Execution) | the workloads' startup, first pass and catch-up, each beside its memory and pauses | alerts only | each milestone, in the release build |

Every other behavior THREAD states, such as the session's results, identity and its misses, the header's checks, the locks and maintenance, is checked by the lanes' ordinary tests, each with the criterion it states.

Some statements are held by argument and review, and no test checks them:

- JITCache adds no threads and never pauses native GC or compiler threads (THREAD's opening).
- An overlapping drain costs only precision (THREAD Capture); the concurrency family checks that it costs nothing more.
- The list of allocations whose failure turns cache activity off is exhaustive (THREAD Failures).
- A design that works only while JITCache stays baseline-only is a defect (THREAD's opening).
- A caller of `delta` holds no JSC-internal lock, a duty JSC cannot check (THREAD Session).
- Power-loss durability, which THREAD Capture excludes.

## Tests

A test checks one case, is deterministic and passes or fails; the cadence below says when it runs. A case is a path the design tells apart, such as a reference kind, an IC state, a request point or a step that can fail. The implementation task that creates a case writes its test and its microbenchmark, and tests the case's combinations with the cases it can meet in one body or one run: defects gather where cases meet, and the task that adds a case knows which ones it meets. Combinations across lanes, at the seams of THREAD's capture and install order, belong to the integrator's tasks.

Each test names the THREAD sentence or SPEC invariant it checks, so a passing test says which claim it supports. Each test also checks that its case happened: an import test, that the body was imported; a fault test, that the fault fired. A test whose case did not happen fails.

Every test runs with strict on (THREAD Verification); a test that also checks the default adds runs with strict off. Tests run in the twins build (`bun build.ts twins`, profile `debug-local-twins`), which compiles the twin checks in (`ENABLE(JITCACHE_TWINS)`, THREAD Execution), and in the plain `debug-local` build, with ASan, LSan and `jsc --destroy-vm`. Tests of the host's own paths also run inside Bun, built with `bun build.ts bun-twins` (harness section 7.7).

A test asserts only what its case determines, so its runs fix the schedule. The runner turns background compilation off (`useConcurrentJIT=false`) in test runs, and each baseline compilation then runs on the thread that runs JavaScript. The image's twin needs that schedule: it recompiles the body from the inputs the producer's compiler read, and a compiler on a worker thread reads them while the script keeps running and can still change them, so what the recording saw and what the compiler read could differ (SPEC-image.md section 11.3). The concurrency family covers the other schedule. A count a test asserts must not depend on when the collector runs or when a time lease expires.

A failure that does not repeat is still a failure, of the code or of the test, and the run that showed it counts as failed; no test is rerun until it passes. The runner repeats on its own only a sequence whose sole failure is an address coincidence inside the heap, which it runs once more (harness section 7.5). The concurrency family runs its scripts many times on purpose, and a single failure among them fails it.

## What the tests observe

Each guarantee maps to one observable. Six families cover THREAD's guarantees today. The rest grow with the tasks: a task that adds a case adds the observable that checks it, in one of these families or a new one.

### The JITCache-off oracle

THREAD Verification, first sentence. After each sequence the runner runs the script again with JITCache off and compares standard output byte for byte, which carries the results, exceptions and stack traces the script prints. In twins builds it also compares the engine's description of the heap JavaScript can reach after a full collection, since a script cannot see closure variables, internal slots or private names (harness sections 6 and 7.6). Every JITCache test checks this besides its own observable.

### Each lane's twins

THREAD Verification's sentences on native twins, relocation, capture records and test builds, and THREAD Execution's rule that a lane lands when its pieces equal their twins. In the twins build each lane checks its restored pieces where they land, against twins the consumer computes and, for state the engine cannot recompute, against the capture record: SPEC-ucb.md section 13.2, SPEC-image.md section 11, SPEC-cb.md section 11.1 and SPEC-ics.md section 11.1. All of them write one twin report, and the report is the observable: a difference fails the run, a relocation coincidence fails it outside the heap and repeats the sequence once inside it, and a skipped check fails the run as THREAD Verification's skip rule says (harness sections 2 to 4, 7.4 and 7.5). Placement and address randomization move every compared domain between producer and consumer (harness section 4).

### The off path emits the pin's code

THREAD Capture: "Other VMs emit unchanged." THREAD Caches allows two exceptions in native code, the `super_construct` store in `JIT::compileOpCall` and the `negate` operations. With no recorder attached, every body of the corpus therefore compiles to the code the pinned engine emits for it, except at those sites (SPEC-image.md I4). The observable compares each body's baseline code from this build and from a build of the pin (`bun build.ts pin`), instruction by instruction, with each address replaced by what it names and the effects of the random draws removed (the entry's `nop`, and constant blinding on x86_64). The pin cannot be instrumented, so both builds describe their code only through what the engine prints, and the comparison names the addresses itself: with the collector off, each address names one object for the whole run, and the two runs agree when they reference the same objects in the same order (harness section 11).

### The skipped warm-up

THREAD's opening paragraph: an import runs "without interpreting the body, compiling it again or relearning its profiles". Per-body event counts show it: bytecodes the LLInt executes, baseline and optimizing compiles, OSR exits, jettisons and reoptimizations. A test asserts the counts its case determines. An installed import, for instance, executes no bytecode in the LLInt and compiles no baseline code, and a case states what it expects of the optimizing counts against the JITCache-off run and the producer's warmed run. The bench reads the same counts on its workloads as trends. Twins builds count each event once per body, where the engine performs it; a script reads the counts of its functions, and the end of a run writes them for every body (harness section 10). Only twins builds count, so the bench reads its counts from twins-build runs of its workloads and its times from release runs.

### Fault injection

THREAD Failures, THREAD Session's rule for failed strict checks, and the crash consistency THREAD Capture gives commits. Each step that can fail is made to fail on purpose: executable allocation (`useExecutableAllocationFuzz`), the producer limit, the store and the writer (the flags of harness section 5.1), invalid material in a body mutated after it was sealed, and, in twins builds, the hooks of the strict checks. The observable: `status` names the failing step, the outcome is the one THREAD Failures gives, a later consumer passes every twin, and the output still equals the JITCache-off run. Each fault test requires its step to happen. Only some steps leave evidence that the fault came before its effects were written: committed bytes show a late DFG-plan fault (a carried deferred baseline counter) and a late IC-handler fault (a given-up site, in a script whose sites cannot give up otherwise), and the image twin shows a late MathIC-snippet fault. The baseline and FTL plans have no evidence beyond their step (SPEC-integrator.md section 15.2). A producer is also killed at each point of a commit where the files change, from before the writer creates its temporary to after the rename (harness section 12). A kill before the rename leaves every body committed before it importable and the interrupted body absent, or its earlier version in place, and the next `clean` reports the temporary if the kill came after the writer created it. A kill after the rename has already committed the body, which the next consumer imports.

### Concurrency

Some of THREAD's statements hold under schedules that a deterministic test fixes away: recording on a JIT worker, a capture while markers and compiler threads drain profiles (THREAD Capture), `delta` with plans in flight, an import beside concurrent DFG compiles, and a producer that commits while another process imports (THREAD Storage). This family runs the corpus with `useConcurrentJIT` on and concurrent collection forced (`collectContinuously`), in the twins build. Its observable is everything that still applies under those schedules: the oracle, every twin check that runs (the image check skips in such runs, harness section 7.4), and ASan. The family passes when `concurrencyRepetitions` repetitions of every script show no failure; one failure fails it, and the task that owns the path turns the failure into a deterministic test wherever a schedule can be forced. A producer that commits while another process imports runs as a Bun-hosted script that spawns the second process with `Bun.spawn`, so the runner still runs one process at a time. ThreadSanitizer is not part of the harness: its target runs with the JITs disabled, where no JITCache path runs. Weak memory ordering cannot show on an x86_64 host or under QEMU, and this family has not yet run on real ARM64 hardware (see Platforms).

## Benches

Benches run in the release build (`release-local`), with strict at its default, off (THREAD Verification), through the same runner, builds and corpus as the tests, with the bench report on (harness section 9). Every number is reported with the process's memory and the pauses JITCache caused beside it (harness section 9.2). The runner holds the build lock for a bench's whole duration, so no build or other run overlaps it, and records the load average before and after. This machine is noisy anyway, so local numbers are signals, and the runner interleaves the runs it compares, so drift spreads across them.

Microbenchmarks give the numbers we can use. A case's script serves its test and its microbenchmark. Each measured span is read in `microbenchProcesses` fresh processes and its minimum is the value, since interference on a shared machine can only add time; the median is reported beside it to show the spread. A microbenchmark never fails a change. Against its own history it raises an alert when its minimum moves by more than `microbenchAlertPercent`. The installation bound is a far stronger and steadier signal than the workload ratios, because the expected gap between an import and a native compile is large.

A workload is a fixed list of units, requests for a server or iterations for a script, and a pass runs every unit once, in order. Each workload's time splits into three parts:

- startup, from spawn to the script's start, `start` included;
- the first pass, where the consumer imports and installs, compared with N1;
- passes 2 to `catchUpPasses`, about 34, where the consumer catches up, each compared with W.

N1 is the first pass of a JITCache-off process. W is the producer's warmed pass: the median of the last five of `catchUpPasses` passes in a Producer process, which then calls `delta` at exit. C1 and Ck are the first and k-th passes of a fresh Consumer on the artifact that producer committed. The ratios C1/N1 and Ck/W are what matter, as signs that something may be wrong; `catchUpMargin`, the ratio within which a pass counts as caught up, is a reasonable number approved from the first measurement, not a gate. W holds optimizing code that this version's consumer compiles natively, so some residual is expected, and the per-body event counts and each lane's bench measurements explain it, beside the two measurements THREAD leaves to the bench: the floor's residue (Restoration) and the samples a process's last `delta` loses (Capture, SPEC-cb.md B4). Each Consumer also runs once with the artifact's pages dropped from the page cache (`posix_fadvise(POSIX_FADV_DONTNEED)` on its files). The workloads are JetStream2's JS-only tests in the jsc shell, a Bun server's cold start (`Bun.serve`), and a short Bun CLI run on fixed input.

An alert fails no change: it opens an investigation, and the milestone's results record what explained it.

The bench also proposes the numbers THREAD leaves to it, which the SPECs name as parameters: it proposes each value from its first measurement, and a value is recorded here only after human approval.

| parameter | what it sets | value |
|---|---|---|
| `installationBoundFraction` | the fraction of the native cost an import may take (THREAD's opening) | unset |
| `installationBoundSmallestBody` | the smallest body that bound covers (THREAD's opening) | unset |
| `defaultProducerLimitBytes` | the producer limit when the host passes none, sized so it stays out of normal runs (THREAD Execution) | unset |
| `writerStagingBytes` | the writer's staging buffer (SPEC-integrator.md IB5) | 64 KiB until tuned |
| `fallbackListingIntervalMilliseconds` | the least time between two listings of an index that cannot read inotify events (SPEC-integrator.md IB2) | 1000 until tuned |
| `benchConsumerProducerGenerations` | the ConsumerProducer runs IB10 chains (SPEC-integrator.md section 16) | unset |
| `kRecordChargeStep` | recording's charge step (SPEC-image.md B2) | 4 KiB until tuned |
| `catchUpPasses` | the passes each workload runs | about 34 until measured |
| `catchUpMargin` | the ratio Ck/W within which a consumer pass counts as caught up | unset |
| `microbenchProcesses` | the fresh processes a microbenchmark's minimum is taken over | unset |
| `microbenchAlertPercent` | the move in a microbenchmark's minimum that raises an alert | unset |
| `concurrencyRepetitions` | N, the repetitions of the concurrency family at a milestone | unset |

## Cadence and results

The implementation workflow has one builder and runner, so builds stay within CLAUDE.md's limits and never run at the same time.

- Every change: the twins build, then the touched lane's directory and the integrator's directory in twins mode, the lane's C++ tests, and the Bun-hosted runs when the change touches a host path (Bun's bindings, or the wiring of `start` and `delta`).
- The end of a task: every directory in twins mode, every C++ test, the plain `debug-local` run, the pin comparison, one repetition of the concurrency family when the task touches its paths, and the microbenchmark of a task that changes a measured span. A task that touches ARM64 code (assembler forms, fixups, patching, islands, ARM64 branches in emitters) also runs the image and integrator directories under QEMU, in both builds.
- Each milestone, one per lane that lands (THREAD Execution) and one for the integrator: the release build's benches and a plain run of every directory in it, `concurrencyRepetitions` repetitions of the concurrency family, and every directory under QEMU. Release code differs from debug code (debug builds add a consistency check around every bytecode), so code that only release emits, such as an ARM64 call and pointer sharing a fixup site, meets the JITCache-off oracle there.

The runner writes each run's results as JSON lines under the build root. Each milestone commits one summary, `docs/JitCache/results/<milestone>.md`, with the commit, the builds and machines, each family's outcome, the bench numbers and each alert with what explained it. A check the cadence prescribes that nobody ran is written down as not run, and a result is written only from a run. A milestone may close with an unexplained alert only when todo.md holds an entry for it with a hypothesis.

## The runner

Tests and benches run through `Tools/Scripts/run-jitcache-tests` and `testjitcache`, which SPEC-integrator.harness.md specifies: the twins build (section 1), the twin report and checks (sections 2 and 3), address-space placement (section 4), the jsc, `bun:jsc` and `$vm` helpers (section 5), the heap description (section 6), the runner's directives, sequences, checks, oracle and Bun-hosted runs (section 7), the C++ test framework (section 8), the bench report (section 9), the per-body event counts (section 10), the pin comparison (section 11), the producer kills (section 12) and the harness's own tests (section 13).

## Platforms

JITCache supports Linux x86_64 and ARM64 (THREAD's opening). x86_64 native, on this machine, is the main loop and runs every family. ARM64 runs here from the cross-compiled aarch64 builds under QEMU user mode (the build table of CLAUDE.md's "JITCache" section): the release build with the sysroot, and the ASan build with the run-time root `~/jitcachearm/arm64-glibc-root` and `ASAN_OPTIONS=detect_leaks=0`, since LeakSanitizer cannot start its tracer under QEMU. QEMU covers ARM64 code paths, fixups, islands and the CPU feature vector, switching the CPU features JSC depends on. Those are the bits the header's CPU vector records on ARM64 (SPEC-integrator.md section 5.2, N11): LSE, JSCVT, FP16, FRINTTS, SHA3 and DotProd, which `MacroAssemblerARM64::collectCPUFeatures` reads from the kernel's HWCAP bits, while `supportsFloatingPointRounding` and `supportsCountPopulation` are always true there. QEMU switches them by CPU model: `cortex-a53` has neither JSCVT nor LSE, `neoverse-n1` (Graviton2's core) adds LSE, and `neoverse-v1` (Graviton3's core) and `max` add JSCVT. The release build cannot run under `cortex-a53`, because the mimalloc it links executes LSE instructions at startup (SPEC-integrator.harness.md N27), so every QEMU run of the cadence goes twice: under `max`, and under `cortex-a53` for the debug builds or `neoverse-n1` for the release build. JSCVT changes baseline code (`JITRightShiftGenerator`) and FP16 the IC cases `Repatch.cpp` caches, and every bit takes part in the header's comparison, so a producer and a consumer under different vectors also check that `start` rejects the artifact.

QEMU does not cover leaks, weak memory ordering, instruction-cache maintenance, which it keeps coherent by itself, address randomization, which the runner replaces with a guest stack size per process (SPEC-integrator.harness.md section 7.9), or performance. Neither LeakSanitizer nor any bench has run on real ARM64 hardware yet.

## Open

- Real ARM64 hardware for what QEMU cannot show: LeakSanitizer, the concurrency family, whether ARM64's allocators move the heap between processes by themselves, and the benches.
