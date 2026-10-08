# HARNESS.md, first version: proposal

Everything here is a proposal for you to decide. Each section says what HARNESS.md would state and what still needs your call.

## 1. Layers

Three documents, each fact in one of them:

| document | holds | never holds |
|---|---|---|
| HARNESS.md | intent: for each THREAD guarantee, the family that proves it, its observable, its oracle, its pass criterion, its cadence and its platforms; the bench's workloads, metrics and approved parameters; the fault coverage table; the results policy | flags, file formats, directive syntax, script names |
| SPEC-integrator.harness.md | runner mechanics: builds, flags, directive grammar, sequences, checks, oracle runs, twin report, placement, pin comparison, kills, bench report | why a family exists or which guarantee it proves |
| lane SPECs | one test table per lane: ID, case, the THREAD sentence or invariant it proves, family, runs and directives | runner mechanics; they cite the harness sub-SPEC |

HARNESS.md's "The runner" section stays as a map of the sub-SPEC's sections, with no content of its own.

## 2. One directive grammar

Today the runner has five expectation directives that grew one at a time: `expect-exit`, `expect-fault` (allows), `expect-no-install` (allows), `expect-twin` (requires and allows), and the proposed `require-fault`. Proposal: two verbs on one axis.

```
// jitcache-allow:   <runs> <outcome>
// jitcache-require: <runs> <outcome>
<runs>    := <run> | <sequence>:<run>
<outcome> := fault <step> | twin <kind> <part-or-domain> | no-install | exit <code>
```

- Without a directive, a run must report no fault, its twin report follows harness section 7.5, a Consumer or ConsumerProducer must install a body, and the exit code is 0.
- `allow` makes the outcome harmless in the named runs: a fault no longer fails, a twin report neither fails nor marks the sequence, zero installs pass, and the exit code may be the one named.
- `require` also makes the outcome mandatory: after the script's last sequence, the runner fails the script unless at least one named run produced it. A bare `<run>` spans every sequence, so `require fault` over a fuzz range checks that each step appeared somewhere in the range. `require no-install` checks a miss path installed nothing, which scripts now verify by hand.
- `exit` and `twin` keep their current meaning under `require`. `exit` takes only `require`.

Migration: `expect-fault` → `allow fault`, `expect-no-install` → `allow no-install`, `expect-twin` → `require twin`, `expect-exit` → `require exit`. The configuration directives stay as they are: `runs`, `host`, `requires: twins`, `check`, `heap: off`, `pin: off`.

Two limits found this round:
- Directives must sit in the first 50 lines, which caps a fuzz range at about 44 sequences. A small script with a lowered `thresholdForFTLOptimizeAfterWarmUp` (a free option) may reach all five sites within that many allocations, but any larger script or sweep hits the cap. Proposal: directives sit in the leading comment block, whatever its length, and `jitcache-runs` takes a range, `// jitcache-runs: <run>; <run> for n in <a>..<b>`, substituting `$n` in the runs' options. SPEC-image.md T12's limit sweep uses the same form.
- Harness section 5.2 does not say what `jitcacheReadSection` returns once cache activity is off. Proposal: it returns `undefined`, and a script that needs bytes reads them in a run whose activity is on.

## 3. Platforms

| platform | runs | proves | cannot prove |
|---|---|---|---|
| x86_64, this machine | every family, debug (ASan and LSan) and release; Bun-hosted runs; the pin comparison | everything THREAD states, on x86_64 | ARM64 code paths, weak memory ordering |
| aarch64 under QEMU user mode, this machine | the release build against the sysroot, and the ASan build with `-L ~/jitcachearm/arm64-glibc-root` and `detect_leaks=0`; the CPU-model matrix (`cortex-a53`, `neoverse-n1`, `neoverse-v1`, `max`) for the header's CPU vector | ARM64 fixups, fixed forms, islands and branch compaction; JSCVT and FP16 code paths; the header rejecting a different CPU vector | leaks, weak memory ordering, instruction-cache maintenance (QEMU keeps it coherent), heap placement (QEMU places the guest's mappings itself), performance |
| real ARM64 | the ASan build with LSan, the concurrency family, the heap calibration and twins relocation, the ARM64 benches | what QEMU cannot | nothing on this list |

Real ARM64 machine: both repos are private, so GitHub's free arm64 runners don't apply, and `bun-debug` alone is 940 MB. Proposal: build here, rsync the binaries, corpus and runner to an on-demand AWS Graviton VM, and stop it after the run. A `c8g.4xlarge` (Graviton4, 16 vCPUs, 32 GiB) costs about $0.64 an hour, so a milestone of a few hours costs a few dollars. Lambda's arm64 runs on Graviton2 and Graviton3, so if Lambda is a deployment target, the ARM64 benches also run once per milestone on a `c6g` (Graviton2); functional runs need only the one machine.

Cadence for real ARM64: every milestone, plus before merging any change to code that runs off the VM thread (recording on workers, capture reads that race native drains, the writer and the store's locks). Each run: the concurrency family N times, the full corpus in the ASan build with LSan, the heap calibration, and, at milestones, the release benches.

QEMU cadence: at the end of any task that touches ARM64 code (assembler forms, fixups, patching, islands, ARM64 branches in emitters), the image and integrator directories under QEMU in both builds; every directory at milestones.

## 4. Fault coverage

Each fault test proves the outcome THREAD Failures gives and that `status` names the step. Only some sites leave evidence that the fault was raised before its effects were written:

| site | step | evidence beyond the step |
|---|---|---|
| baseline plan | `exec-alloc.baseline-plan` | none: the body never runs baseline again, so no capture can hold it |
| DFG plan | `exec-alloc.dfg-plan` | committed `cb.state` holds no carried deferred baseline counter |
| FTL plan | `exec-alloc.ftl-plan` | none: a cleared quick FTL bit is also a native state |
| IC handler | `exec-alloc.ic-handler` | in a script whose sites can only give up through the fault: no `ICsBaseline` record with `tookSlowPath` or a given-up site |
| MathIC snippet | `exec-alloc.mathic-snippet` | the image twin replays the regeneration; a captured missing snippet fails it |
| JITCache's image | `exec-alloc.jitcache-image` | the import fails, the body runs native, and the oracle holds |
| producer limit | `budget.limit` | nothing committed after the fault |
| store and writer | `writer.*`, `container.io` | the kill and writer-fault tests' file states |
| invalid material | `ucb.*`, `image.*`, `cb.*`, `ics.*` | activity off at the named step, oracle holds |

`require fault` over the fuzz range checks that each executable-allocation step appears at least once. The baseline-plan and FTL-plan rows rest on the step alone, and HARNESS.md says so.

## 5. The bench

Timing: per-call times are wall time (`CLOCK_MONOTONIC`); the installation bound's total is the VM thread's CPU time (`CLOCK_THREAD_CPUTIME_ID`), as THREAD defines it. Parameters are proposed by the bench from its first measurement and recorded in HARNESS.md only after your approval.

"A consumer's first run matches the producer's warmed run", made exact:
- A workload is a fixed list of units (requests for a server, iterations for a script). A pass runs every unit once, in order, timing each unit with `CLOCK_MONOTONIC` and the pass from process start.
- Three runs, each in fresh processes, release build, strict at its default:
  - N1, the first pass of a JITCache-off process;
  - W, the producer's warmed run: the second pass of a Producer process, after a first pass that compiles and captures. The producer then calls `delta` at exit.
  - C1, the first pass of a fresh Consumer process on the artifact that producer committed.
- "Matches" means C1 ≤ (1 + ε)·W, with ε a bench parameter (`firstRunMatchMargin`) you approve per workload class. The report gives C1/W, N1/W, the share of the native warm-up gap JITCache closes, (N1 − C1)/(N1 − W), and the first unit after which the consumer's per-unit times stay within (1 + ε) of W's.
- Each number is the minimum over the bench's process count, with the median beside it.
- W includes optimizing code that C1 must compile natively, since this version transports only baseline. The residual C1 − W is explained by the event counts of IB11 and the measurements beside them: LLInt bytecodes in imported bodies (should be zero), baseline compiles avoided, DFG and FTL compiles and when they happened, exits and jettisons, IC slow-path visits (ICs B4), install CPU (IB1 to IB3), the floor's residue and the samples the last `delta` lost (CB B4).
- Page cache: C1 runs with the artifact warm, and once with it cold, dropped with `posix_fadvise(POSIX_FADV_DONTNEED)` on the artifact's files, which needs no root.

Macro workloads, to choose:
- JetStream2's JS-only tests in the jsc shell, the corpus the earlier measurements used;
- a Bun server cold start (`Bun.serve` with a framework app), measuring time to first response and the first k requests;
- a short-lived Bun CLI run on fixed input (a TypeScript compile or a lint pass), the serverless-shaped case.

## 6. The rest of HARNESS.md's Open list

- N and X become bench parameters you approve: `concurrencyRepetitions`, `microbenchProcesses` and `microbenchWarnPercent`, proposed from the first measurements.
- Results: the runner writes JSON lines under the build root; each milestone commits one summary to `docs/JitCache/results/<milestone>.md` with the commit, builds, machines, each family's outcome, the bench numbers and each warning with its explanation. A check nobody ran is written as not run.
- Overlapping runs (a producer committing while another process imports): Bun-hosted scripts spawn the second process with `Bun.spawn`, so the runner needs no parallel-run grammar.
- Bun-hosted runs: on every change that touches a host path (Bun bindings, `start` and `delta` wiring), otherwise at the end of a task.
- A quiet machine for benches: the runner takes the build lock for the bench's whole duration, so no build or other run overlaps, and records the load average before and after. The installation bound uses thread CPU time, which shared load disturbs least.
- Unexplained warnings: a milestone may close with one only if it has a todo.md entry with a hypothesis. A failed installation bound is a failure, not a warning, and blocks.
