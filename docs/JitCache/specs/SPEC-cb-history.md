# SPEC-cb history

Why [SPEC-cb.md](SPEC-cb.md) decides as it does where a more obvious design came first. Nothing here binds; where the two differ, the SPEC wins.

## What stays local

THREAD makes state that DFG or FTL code sets wait for the DFG capture, which is why `m_hasBeenCompiledWithFTL`, set only by the DFG's FTL tier-up, stays out (section 1). Read literally, the same rule would also hold back the lazy-operand profiles (P3) and the reoptimization count (P11), since only OSR exits and optimizing-tier events write them. THREAD names both as travelling, and the explicit statement wins over the general rule, as it does for the exit sites and quick tier-up bits the UCB lane carries. `m_osrExitCounter` counts exits of an optimized CB, never of a baseline one. Every bucket, the standalone failure buckets included, holds only pending samples, and a drained bucket is empty, so leaving buckets out loses nothing a drain would keep. `ToThisClearedByGC` travels as scalar history, like a call site's cleared-by-GC bit, while the structure cached beside it is a cell cache that starts at its link state.

The array profile of `get_by_val_with_this` first travelled as a seventeenth array family, A16, because the IC slow operation classifies structures into it, which looked like learned state. Nothing outside that classification reads it (N2): `CodeBlock::getArrayProfile`, the route of the DFG and of OSR exit, skips it, the DFG's case reads only `GetByStatus`, the LLInt ignores it, no merge walks it, and the one read of its modes writes straight back into it. THREAD keeps local what the consumer recomputes before anything reads it, so the profile stays native. Richness agrees, since THREAD counts what the native merge leaves for the DFG, and the DFG never sees these flags. Dropping A16 renumbered the families after it, which is why the allocation hints start at F16.

## Dense, positional sections

Every field the lane carries sits at a position a native index space fixes: the argument index, the value-profile offset, and metadata-ID order within one opcode's metadata. Keyed sparse records would have saved bytes only for cold sites, and a body is captured after its baseline compile, when most sites have run. Dense arrays in native order won, because they make validation a count comparison (V2 to V4) and seeding a walk in step with the metadata, with no search and no key decoding.

The families are a fixed table generated from the native macros, rather than (opcode, field) pairs stored in the file. The header's build ID already pins opcode values and metadata layouts, and generating the table from the macros the native merge walks keeps the two orders equal by construction.

## Two sections

Scoring reads only the summary, and only at a key's first scoring, so a section of its own lets the glue read a few hundred bytes instead of the whole state. The summary keeps per-slot categories because THREAD lists them as summary content, and the score is derived from them at decode, so the file carries no scalar that could disagree with them. One progress value serves both the summary's last tie-break and the envelope's P, since THREAD Maintenance already defines P, floored at zero and capped at the active threshold.

Both layout versions stay although nothing fails on them today: every format the container and the UCB lane define carries one, and dropping this lane's alone would change its header for a check THREAD does not ask to remove.

## Bodies without a metadata table

The lane first counted zero entries for a CB without a metadata table but read the table directly everywhere else. `UnlinkedMetadataTable::link` returns null for a body whose bytecode adds no metadata entry and no value profile, `MetadataTable::forEach` and `valueProfileForOffset` fault on a null table, and such bodies still reach baseline with argument profiles and a counter (`function id(x) { return x; }`). A producer would have crashed at the finalize capture of every hot function without metadata, and a consumer when it installed one. Every native walker tests the table first, and the lane now does the same through two guarded walkers used for every access (section 3.1). The ICs lane's equivalent walker was offered and declined, so that neither lane's tasks wait on the other's header.

## Validation is strict-only

The V-rules first ran always, as safety bounds on indexes, enum cases, shift widths, the engine's constructor assertions and array shapes, and only the plausibility checks (the newborn link state, non-negative progress under a finite threshold) were strict, so that a native behaviour the SPEC had not modelled could not turn cache activity off for every user. THREAD Session then made normal mode check only the artifact's integrity and trust the rest, because an artifact that carries machine code must be as trusted as the binary that runs it. Every V-rule, summary rule, S-check and SC-check now runs with strict alone.

In normal mode a broken pairing with the UCB copies fails as it does natively, through the hardened `std::span::operator[]` the native merges use. SC2 first ran after the reads it guards; it now runs before them, and stays strict because normal mode trusts the glue's eligibility decision. A saved summary that fails validation is invalid material, since it is bytes read from the artifact; the capture's pairing guard is a recording fault, since it rejects what a capture builds from the producer's own state.

## The new_array_buffer hint bound

V8 first accepted any copy-on-write type in F19. A hint below the literal's own type makes `slow_path_new_array_buffer`, or OSR materialization in the FTL, copy the literal into a butterfly of the hint's type without converting, so cells land in an Int32 butterfly the collector never scans, or turn into doubles (N8).

Reading the newborn CB's own linked hint at `prepare` was proposed and rejected: it equals the instruction's type only while the CB is at its link state, and SC1, which tests the live CB's hint, needs the instruction anyway. That first bound also allocated per-site scratch that the producer budget never charged. An interim rule rejected an F19 entry that no instruction or two instructions named; it was dropped, because the first is never read, the second is checked against both, and native bytecode produces neither (N8).

Once V8 checks every instruction, the one-to-one mapping looks like a fact no rule needs. S2 and V8 at SC1 need it, because both must accept every native CB. A shared entry would hold only the type its last instruction linked, and an entry no instruction named would keep the zeroed bytes of N1; either would make S2 or V8 reject native material. N8 therefore states the mapping, and the WebKit bump review rechecks it (section 9.3).

The intent is a per-instruction lower bound by `m_recommendedIndexingType`, walked in place with no scratch, and one predicate for `prepare` and SC1.

## Iteration-mode masks

V9 first bounded every iteration-mode value by the 15 bits `IterationMode` defines. The DFG cannot parse every such bit at a sync site: `handleIteratorOpen` and `handleIteratorNext` emit one case per set bit, chain each fast case but the last to a `failedBlock` that only a later case consumes, and assert that none is left, so a bit with no case leaves a block without a terminal (N9). The intent is each opcode's native writer mask.

A reviewer proposed `0x7fff` for the async opcodes. It was declined because their handlers mask to the same bits the writers record, and the narrower sets also keep stray bits away from `canUseFastIterationMode`, which counts every bit other than `Generic`. No cap applies at `maxNumberOfFastIterationModes`: the DFG emits a case per bit, and the runtime only degrades new modes to `Generic` once two are seen, so more bits are a superset the engine handles.

## Lazy-operand keys

V13 first bounded tmps by `maxNumCheckpointTmps`, and a strict S4 checked each key against the CB's own frame, on the premise that keys index that frame. A key's operand is the exiting `GetLocal`'s, remapped into the machine frame of whichever compile inlined the CB (N7), so an inlined CB holds locals past its own `numCalleeLocals()` and shifted tmps, and private tmps start past `numTmps()` even in the root frame. With strict on, SC1 would have ended production at the first `delta` of an inlined CB that saw an exit. No check against the CB can tell such a key from a forged one, and the engine only hashes and compares keys, so V13 keeps only what the key's constructors assert plus the absence of constant registers. A ceiling such as the 10-bit `tmpOffset` range plus `maxNumCheckpointTmps` was declined, because an inlinee's private tmps add to it without bound.

The keys refer to no other lane's content and to no other body: the bytecode index belongs to the profiled CB, which the UCB lane's index-space guarantee covers, and the operand is an opaque key the consumer's DFG matches only when it builds the same inlining layout.

Two later additions were undone. V13 tested keys for uniqueness, which needed scratch the budget never charged; capture then sorted the keys so that V13 could demand strict order instead. Both went, since no reader needs unique keys: `LazyOperandValueProfileParser::initialize` keeps the first profile per key, `addOperandValueProfile` returns the existing one, and seeding through it collapses a duplicate. A restore-only append was also added to avoid `addOperandValueProfile`'s linear search, then removed: n is one CB's distinct exit keys, which OSR exit already searches the same way, and the extra engine edit had nothing to measure against.

## The counter travels raw

An early draft re-derived `setThreshold`'s arithmetic in JITCache to compute the consumer's slice. Handing the restored counter to `ExecutionCounter::checkIfThresholdCrossedAndSet` instead removed a copy of engine logic that a WebKit bump could silently diverge from. The infinite threshold stays out of that call, because `setThreshold` would call `deferIndefinitely` and drop the progress. `handleExitCounts` can leave an infinite threshold with a positive total, so the case is reachable; its captured slice keeps the producer's ceiling, which only decides when the next check runs.

I1 first claimed byte-identical captures across processes. The split between `m_counter` and `m_totalCount` holds the producer's memory-pressure slice, each unclipped re-slice adds a fraction to P, the multiplier depends on the pool, and drains run at GC-driven times, so neither the counter's split nor the predictions reproduce. A record of P and T alone was considered, but P is `double(float) + int32`, and an arbitrary P cannot always be split back into a float and an int32 that sum to it exactly, while the raw pair is exact by construction. The raw triple stayed, I1 states the structural property instead, and U4 checks that two splits of the same P restore the same counter.

The twin first re-ran the native check, but its multiplier reads the pool count, an atomic that JIT workers and other VMs change at any moment, so a recomputation could arm another slice or flip the crossing. `finishCounter` now returns what it decided, and the twin compares that with an envelope no pool can move, since M is at least 1 and C depends only on options and the CB. A return value was preferred to a `mutable` field or an out parameter, because it keeps `finishCounter` `const` and the import a read-only view of its span, and the same record feeds the bench.

## Polymorphic bodies carry no counter progress

At first one floor of two entry increments served every body, marked provisional. After an import, a non-looping body's first DFG compile then saw a single invocation of IC evidence: the ICs lane restores no case, so each site lists at most the case that invocation met, the DFG speculates on it, and the `BadCache` exits on other shapes enter the UCB's exit profile. Richness counts exit sites, so a ConsumerProducer's recapture won and carried those sites to every later generation. A larger floor for every body would have delayed the first DFG compile of bodies whose first invocation already caches every shape.

THREAD settled it: a body whose captured property IC lists two or more cases carries no counter progress, the ICs lane supplies that bit at capture, setup's native arming stands, and the floor still serves bodies that carry progress. The score carries `counterWithheld` because THREAD ranks a withheld counter above one that travels, just before progress. Its encoding, `NotCarried` as 0 in a header byte that used to be reserved, is safe because the header's build ID keeps any binary from reading a summary written while the byte was reserved.

## Tier-up history before setup, aging after

Setup's `optimizeAfterWarmUp` reads the reoptimization count, so P11 lands before setup, and setup arms the threshold native code would arm for this history, which a counter that does not travel keeps. P10 lands in the same step, which costs nothing and keeps the CB's scalars together.

Setup samples aging from a fresh counter, so restored progress would look like activity at the next old-age check and renew a lease the CB never earned. `finishCounter` resamples right after writing the counter; the field is CB heuristic state, so this lane owns it. The twin has to read the private sample, hence M1's twins-only getter, and compares it with `float(count())`, because `snapshotExecutionCounterForAging` narrows to float and the old-age check compares in float.

## The realm step

A seeded `FastMap` or `FastSet` bit at an `iterator_open` site trips the DFG's assertion in a realm that has not yet created a `Map` or `Set`, and a release build freezes the empty value, so the site exits on every run, a jettison churn the JITCache-off run never has (N10). Stripping the bits would drop state THREAD carries and break the twin, and a DFG edit that masks a bit whose function is missing would change the engine for a state native code cannot reach. The intent is to call the native lazy getters under the install deferral, only for a seed that carries the bit, which restores the native invariant that the bit implies the function. THREAD forbids cell allocation only during capture, so allocating here is the lane's decision.

Every other realm read behind the seeds was checked. The other iterator handlers read functions and structures the realm creates eagerly, and typed-array modes need no step, because the DFG answers a missing typed-array class with `CheckArray`.

## Twin checks run before installCode

The twin check first ran after `installCode`. From then on a concurrent marker can reach the CB, merge the UCB copies into P1 and P2 and drain P3 (N11), and the UCB copies an import restores often hold bits the CB copy lacks, so a check after publication would fail at random. It runs between `finishCounter` and `installCode`, the only point where the seeded values are exact.

## The DFG plan-site fault comes first

A DFG plan that runs out of executable memory finalizes as a failed compilation, and its callback defers P12 through `dontOptimizeAnytimeSoon` while the baseline CB stays its executable's replacement. A later `delta` captured that deferral, which V15 and S3 accept, so every consumer importing the body would have kept it out of the DFG. No part owned the fault at that site until THREAD Execution gave the plan sites to the integrator; R-INT-12 keeps the order this lane needs, the fault before the callback.

## Concurrency runs

The concurrency subject first asked every twin to pass beside concurrent DFG compiles. The Image lane's check cannot: with `useConcurrentJIT` on, the LLInt keeps writing what a worklist compile reads, and in the consumer the check overwrites arithmetic profiles that compiler threads read without a lock. The Image check now tests its own preconditions and skips, and this lane runs two cases: one with concurrent marking and synchronous JIT, where every twin passes, and one with concurrent JIT on as well, where this lane's twins and the oracle still hold. Per-lane twin switches in the runner were declined, because a run that forgot one would leave the Image check unsound.

## The bench needs no lane hook

The floor's residue and the time to the first DFG compile were first promised as lane bench items, though both happen outside every lane call, at the first `operationOptimize` crossing and in `DFG::compileImpl`. Native logs already report both under options that leave baseline emission alone (`verboseOSR`, `logCompilationChanges`), and hooks in `operationOptimize` and elsewhere would put bench instrumentation into files no lane owns, for a measurement THREAD gives the bench loop. The glue times the lane's own calls and hands the bench each import's `CBCounterRestore`.

## Tasks and edit ownership

E1 to E3 add accessors to classes no other lane edits, while `CodeBlock.h` is shared, so its two additions go through the integrator's manifest (M1). Concurrent tasks first wrote the same files, and no task owned the shared headers or the validators that SC1 and `prepare` both run; task 2 now writes both headers and the validators, and each later task adds only its own `.cpp` file and test file.

The twin check and the corpus first shared a task that waited for the integrator's install glue, which in twins builds waits for the twin check. `verifyTwins` and its unit tests now land first, and the corpus after the glue and the runner. The corpus needs no Bun host, because every case runs in the jsc shell, the realm step through the shell's `createGlobalObject`.
