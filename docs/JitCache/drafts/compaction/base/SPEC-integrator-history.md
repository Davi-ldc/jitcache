# SPEC-integrator history

Rationale and review records for [SPEC-integrator.md](SPEC-integrator.md) and its sub-SPECs. Nothing here binds.

## Draft 1

Derived from the four frozen lane SPECs and from THREAD after its revision that settled the lane authors' gaps (strict failures, the summary writers, the module names, contexts in identity, the registry outside the producer limit, the VM data images avoid).

### Where the integrator meets each lane

Every requirement a lane places on the integrator was mapped to a section before anything else was written: UCB R-INT-1 to R-INT-11, Image R-INT-1 to R-INT-11, CB R-INT-1 to R-INT-12, ICs R-INT-1 to R-INT-9, and the four manifests. Section 15 of the main file applies every manifest at once, with stubs, because a lane implementer may not edit `Sources.txt` and still needs its files compiled from the first task on.

Two manifest entries were conditional. The Image census names `JIT::compileOpCall` in its row E2, but the helper it changes is `CallLinkInfo::emitFastPathImpl`, so only the ICs lane edits `compileOpCall` and E1 needs no manifest. The UCB lane adds `ArithProfile::restoreBits` in the header and the Image lane changes `emitUnconditionalSet` in the `.cpp`; different functions, so each lane makes its own edit.

### The install function

THREAD's two install points became one function with a point argument. Before `setupLLInt` it replaces the LLInt setup and calls `installCode` itself, as THREAD's order requires (the CB lane's twin check must run before `installCode`, and the import's cost is measured "until JITCache returns after `installCode`"), so `prepareForExecutionImpl` skips its own trailing call after an import. In `JIT::compileSync` the import replaces the plan; `prepareForExecutionImpl` then calls `installCode` a second time, which the LLInt-off route already does natively. The function repeats the sharing-slot and newborn tests that its callers made, because `compileSync` has callers that make neither.

`shouldJIT` was file-static in `LLIntSlowPaths.cpp`. Giving it external linkage, unchanged, is the smallest edit that lets the gate be the LLInt's own.

### Fault entry points

The lanes wrote `didFailExecutableAllocation(VM&)`, and each of their requirements asks for a diagnostic naming the site. A one-argument function cannot name it, so the integrator, which owns the API, added the site. The entry point records the fault with stores only and frees nothing: the ICs lane calls it under a `CodeBlock::m_lock` held through `GCSafeConcurrentJSLocker`, and the Image lane forbids waiting for another thread. Production memory is released at the next glue entry instead.

`status` runs on the VM thread. That keeps every fault record a plain VM-thread field; the only cross-thread writer, a debugger attach, sets atomics that `status` folds in.

### File names

THREAD's layout names `<body-key-hash>.bin`. The 40 key bytes are already a digest (a header of kind, specialization and mode, then a SHA-256 identity digest), so the integrator names the file by the key's hex and adds no second hash. A second SHA-256 would run at every request point and every CodeCache hit, since the UCB lane calls `bodyVersion` there; one compression of a 64-byte block costs a few hundred cycles in software, so tens of thousands of requests in a large app's startup would spend milliseconds on names alone. With the key in the name, the index is keyed by the key itself, which makes it exact: a mismatch between a file's name and its envelope is damage, never a collision.

### Reading bodies

Bodies are mapped `MAP_PRIVATE | MAP_POPULATE` and validated in full at every open. A mapping shares page-cache pages, which the kernel can reclaim, where a read into private memory would hold a second copy for as long as a pending import pins the body; installation memory comes before serialization time in THREAD's priorities. The cost is the fault a truncated file raises. JITCache never truncates a published body, and the artifact is trusted input anyway, since it carries machine code; the container sub-SPEC states that model in section 7.3.

CRC32C rather than SHA-256 guards each section because every import validates the whole file and the checksum guards against damage, not against an adversary. With SSE4.2 or ARMv8 CRC instructions it runs at several GB/s.

Commit identifiers are random 64-bit values. A counter would need shared state that survives artifact deletion and lock-file loss; a random value needs none, and two writes collide with probability 2^-64. The UCB lane only needs "another version", never an order.

### Seeing other processes' commits

The UCB lane expects "a body committed later, by this VM or another producer" to be tried at the next request, and R-INT-3 forbids a filesystem call for an absent body. Polling the directory, or an inotify read per lookup, would put a system call on every request point. The lock file already exists and is never replaced, so it carries a commit epoch that producers and maintenance bump after each publish or removal; a consumer compares one shared word per lookup and drains inotify only when the epoch moved. Producers need neither: they hold the lock, so nobody else commits while they run.

The index is listed at `start` rather than at the first lookup. "Once the artifact is open" in R-INT-3 then holds from `start` on, and the listing is the demand a consumer role creates; a lazy listing would put a directory scan inside the first request point.

### Scoring and kept summaries

THREAD reads the saved body's summary "at the key's first scoring". The integrator keeps it afterwards and replaces it at each commit with the score computed from the sections it just wrote, so a key never costs a second read while the VM holds the lock. Kept summaries are production memory and are charged; the charge of four bucket sizes per entry bounds a hash table's storage and its rehash peak without depending on WTF's growth policy.

`delta` scores in two phases. Phase 1 reads under `CodeBlockSet::m_lock`, which `forEachCodeBlockIgnoringJITPlans` requires, and keeps the best candidate per key; phase 2 reads saved bodies and writes files without that lock, because no file I/O should run under a lock the CodeBlock constructor takes. The CB pointers kept between the phases stay valid because nothing can complete a collection while the VM thread holds heap access, reaches no stop point and allocates no cell.

### Bun

Bun calls `start` in `Zig__GlobalObject__create`, right after it takes the API lock and before `JSVMClientData::create`, which is the last point before anything could create a builtin executable. Configuration comes from `BUN_JITCACHE_*` variables, read once per process, like `BUN_JSC_*`. A worker of a process whose main VM produces gets `Busy` and falls back to `Consumer`, so workers still import what the main VM commits.

Bun calls `delta` in `VirtualMachine::on_exit`, after the user's exit handlers and before teardown, the same point where it persists the Node compile cache. THREAD's "no implicit `delta`" constrains JITCache; the host chooses when to call it, and process exit is the idle point a serverless instance reaches.

### Test builds

The runner reconciles two requirements. The Image lane's twins runs turn concurrent compilation off and fail on a skipped image check; the ICs lane's runs keep the default and treat a skip as no difference. Both hold when a skip fails a run exactly when that run has concurrent compilation off, with each directory choosing its default. The human settles whether that reading is right (main file, P3).

The Bun-hosted tests of the UCB lane run with twins on, so the twins profile builds the Bun executable position-independent too, extending the Image lane's provisional choice to the binary that holds the engine in Bun (main file, P2).

## Review records

### Round 1

Seven findings, four blockers and three majors; all held against the code and THREAD. Two of the blockers describe one defect.

#### Bun's main VM has execution context id 1 (blocker)

N15 had copied the comment above `Zig__GlobalObject__create`, which is stale. `VirtualMachine::init` derives `context_id` as `opts.context_id`, else 1 for the main thread and `i32::MAX` otherwise. Workers pass `worker.execution_context_id()`, which `WorkerMessagingProxy` takes from `ScriptExecutionContext::generateIdentifier()`, a counter that starts at 1 and pre-increments, so the first worker gets 2. The debugger thread's VM and macro VMs pass no id and get `i32::MAX`, and `Zig__GlobalObject__create` itself tests `== INT32_MAX || > 1` for "not the main thread". A host written from the old N15 would have suffixed the main VM's report paths with `.1`, and the runner would have found no twin report at the configured path. N15 now states the derivation, and section 14.2 classifies by value: 1 is main, 2 to `INT32_MAX - 1` are workers, anything else is not configured.

#### `testjitcache` set options before they existed (two blockers, one defect)

`Options::initializeWithOptionsCustomization` resets every option with `INIT_OPTION` inside its `std::call_once` and constructs `g_optionWasOverridden` in `OptionsHelper::initialize`. An `Options::setOptions` call before `JSC::initialize` reaches `OptionsHelper::setWasOverridden` on an unconstructed `LazyNeverDestroyed`, whose access asserts `m_isConstructed`, and in a build without assertions the reset then overwrites the group. Two fixes were offered: apply the group inside the customization callback, or follow the jsc shell (`Options::initialize` under `AllowUnfinalizedAccessScope`, then `setOptions`, then `JSC::initialize`). The callback form was taken. It is how Bun's `JSCInitialize`, `testb3` and `testmasm` set options, it puts the group where `Options` gives the embedder the last word, and `notifyOptionsChanged` runs after it as for any embedder. The shell's order is equally correct but splits initialization in two for no gain here. The second finding's test suggestion was kept: H3 now reads a group's options back, a derived one included. The same finding noted that `VM::~VM` asserts `currentThreadIsHoldingAPILock()`. `JSLockHolder::~JSLockHolder` drops its own VM reference before it unlocks, so the harness drops the test's reference inside the holder's scope and the holder's reference is the last, as `runJSC` does under `--destroy-vm`.

#### The writer parsed lane formats (blocker)

SI1 mapped every committed file and ran seven lane parsers, and the writer computed the kept score from the file. THREAD Storage makes sections "opaque bytes to the container, which only locates and checksums it", THREAD Capture asks the writer to "reread and validate" the file, which the container validation already does, and THREAD Session leaves to each SPEC which assumptions strict covers. No lane asked for SI1. The CB lane already checks its own summary at capture (SPEC-cb.md SC1), and the UCB, Image and ICs lanes chose capture checks that do not round-trip their bytes. SI1 therefore overrode three lanes' choices, repeated the CB lane's, mapped and parsed every strict commit inside the capture pause, which THREAD ranks second, and tied the writer to every lane's format. The reread compares each section with the checksum computed from the very bytes the lanes built, so parsing the file proves nothing that parsing those bytes would not.

SI1 is gone, and the writer touches no lane format. The kept score moved to the glue (section 9.4, step 6), computed from the in-memory summary bytes by the same three readers that score a saved body. It runs before the writer, so a reader that rejects its own lane's fresh bytes stops the commit as a recording fault; publishing those bytes would have made every later scoring of the key, and for `ucb.feedback` and `ICsBaseline` every import, meet invalid material. This is no strict check, since the score needs the readers on every capture. Its class follows THREAD Session's rule for a check that guards a capture.

#### Task 1 could not reach its own acceptance (major)

T1 applied every manifest entry at once and stubbed only `Sources.txt`'s files. The `VM::~VM` calls referenced functions task 2 defines, `$vm`'s two functions called UCB code from tasks 6 and 10, `testjitcache_SOURCES` named ten test files no task had written, which CMake rejects at generation, and M2 exported UCB headers that exist only after the UCB lane's task 0. The CMake snippet also stopped at `WEBKIT_EXECUTABLE_DECLARE`, which only creates the target around `cmakeconfig.h`; `testFFI` gets its sources from a later `WEBKIT_EXECUTABLE`. Section 15 now gives each hot-file entry exactly one task, after the tasks that write what it references: the `VM.cpp` calls go to task 2 (M4), `$vm` to task 12 after the UCB lane's tasks 6 and 10 (M7), task 1 waits for the integrator's and the UCB lane's task 0 and stubs every test file the target lists, and the target gains its `WEBKIT_EXECUTABLE` beside `testFFI`'s. The entries that name no hot file moved to a table of their own, which says where each is met.

#### The oracle had no way to compare the reachable heap (major)

THREAD Verification names four observables. Scripts can print results, exceptions and stack traces; closure variables, internal slots and private names are invisible to reflection, and a walk written in JavaScript would run getters and traps. The CB lane's corpus compares the heap and its realm step argues soundness from it. The harness now has a description of the heap JavaScript can reach (harness sub-SPEC section 6), written by the jsc shell at the end of every twins-mode run and compared by the oracle (section 7.6).

The design follows from what must not differ between two runs JavaScript cannot tell apart. Tiers, ICs, structures, storage formats and addresses must not show, so the walk uses `getOwnPropertyNames` order and `VMInquiry` slots and prints no layout. The collector's conservative stack scan can keep a weakly held cell alive in one run and not the other, so weak edges never give a cell an ordinal. Native hash orders are not canonical, so scope variables sort by name and weak entries by their keys' ordinals. Two exclusions follow from how runs are made. The shell's `arguments` holds the role, which differs from the Off run by construction, so the description leaves it out and scripts hold their bodies in a function of the role. Bun's global object holds per-process state, so Bun scripts describe roots of their own. The description runs after the exit-time `delta`, so its full collection, which drains profiles and can age code, changes no capture.

Two kinds of state are listed without being walked. A finalization registry's held values that only the registry reaches sit in hash-ordered lists, so two runs could not agree on whose subgraph to number first. A pending promise's reactions have a native form, inline in the promise or spilled into `JSPromiseReaction` cells, that JavaScript cannot see. JavaScript observes both only when the callback or the reactions run, and then through what they do, which the rest of the output and the final description show; when the run ends they never run. Walking them would need a canonical form for unordered subgraphs and a normalization of the reaction forms, for nothing a run can observe.

#### A transient open failure scored a saved body as absent (major)

`openBody` returns `Missing` for `EMFILE`, `ENFILE` and `ENOMEM`, which is right for the UCB lane: a body it cannot read now is a miss and generation proceeds. The capture glue read the same `Missing` as "no saved body", which any candidate beats, so a descriptor shortage at a key's first scoring could let a poorer capture replace a richer body, against THREAD Capture's "when it beats the saved body". The glue now has a scoring read of its own (container sub-SPEC section 7.3) that returns `Absent`, `Unavailable`, `Unusable` or `Found`, and `Unavailable` defers the key without a fault. The read also stopped mapping and checksumming whole bodies: it checks the envelope, the directory and the three summary sections, which is all a score reads. A key's first scoring in a ConsumerProducer no longer costs a pass over its image inside the capture pause, and imports still validate everything they map.

### Other changes in round 1

The site argument of `didFailExecutableAllocation` had been marked provisional (old P4). THREAD Execution gives API signatures to the integrator, and the lanes call their names working names, so the argument is this SPEC's own decision and its reasoning now sits in section 5.4.

### Round 2

Four findings, one blocker and three majors. All four describe real defects; for the third, the fix the finding preferred was not taken.

#### Every listing after the first read an empty directory (blocker)

The container listed `bodies/` through `fdopendir(dup(bodiesFd))`. `dup` shares the open file description and its offset; glibc's `fdopendir` does not seek, and `readdir` continues `getdents64` from the description's offset. The listing at `start` left `bodiesFd` at the end of the directory, so every later listing (a refresh without inotify, after `IN_Q_OVERFLOW` or after an inotify read error) returned nothing, and the index emptied for the rest of the process. A test program written for this check listed a directory of five files through `fdopendir(dup(fd))` twice and got 7 entries, then 0; `fdopendir(openat(fd, ".", O_RDONLY | O_DIRECTORY))` gave 7, and so did `dup` followed by `rewinddir`. The refresh also cleared the index before listing, so a listing that failed left it empty, against the error table's own row.

Each listing now opens `.` relative to `bodiesFd`, an open file description of its own at offset zero, builds a new map and swaps it in only when it succeeds. `openat` was preferred to `rewinddir` because it shares no offset with any other user of `bodiesFd`. Every listing drains the inotify queue, reads the epoch, lists and records that epoch, so nothing between the drain and the listing is lost. A refresh whose listing fails keeps the old index, records the epoch and marks a listing pending, so the next epoch change lists again: the drained events are gone, and reading new events alone would not restore them. Retrying at every lookup instead would put a system call, or a whole listing, on every request point while the failure lasts, to recover bodies whose absence costs only a miss. C4 now lists one object again and again, after two overflows and with inotify off.

#### The task plan scheduled tests and calls before the code they need (major)

All six points held. T-FAULTS needs a VM that `start` configured and `status`, so it moved to task 7. T-BUILDID's `start` clause moved into T-START. C5 now tests the lock as a store object, `ProducerLock`, which task 5 writes; the publish clause moved to C7 with the writer, and the release on a VM's destruction to T-START. T-DELTA reaches `delta.reentrant` through a twins-only capture probe (main file section 9.6), a function the glue calls right before each `commitCapture`; nothing else calls out of a capture. The `BenchReport` itself (open, `record`, flush, `benchReport(VM&)`) moved to task 2, so tasks 5, 7, 8 and 9 record their own events, and task 16 adds only the native-cost hooks and the `compile` event. The store's and the writer's twins hooks live in `ArtifactStore.cpp` and `ArtifactWriter.cpp`, the files of the tasks that implement them, and `JITCacheTwinsHarness.cpp` no longer claims them. Task 14 now comes after task 12, which defines the twins host functions and the placement functions the Bun host calls.

#### Shared opened artifacts did not refresh by role (major)

The defect held. The object decided at its build, from the building VM's role, whether it followed the epoch and watched the directory. Consumers sharing a producing VM's object therefore never refreshed, and a producing VM sharing a Consumer's object inherited an index whose completeness, which its scoring read needs, rested on an unstated refresh.

The fix the finding preferred, one object per VM, was not taken. Bun runs a VM per worker, and the workers of a producing process are the Consumers of its main VM's artifact (main file section 14.2). Per-VM objects would list the directory at each worker's start, keep an index per worker (up to four 48-byte buckets per body in WTF's hash table, a few megabytes per worker for tens of thousands of bodies), take an inotify instance per worker from the user's 128, and make every worker refresh after each of the main VM's commits. Where inotify instances run out, every such refresh lists the whole directory, once per worker and per commit. One shared object costs one listing, one index and one instance per process, and the main VM's commits reach its workers at once through the writer's index update.

The finding's second fix was taken, changed in two places. Every object follows the epoch and watches the directory, whatever the roles of the VMs that hold it. The scoring read does not refresh. Instead, a producing VM whose `ArtifactRegistry::take` returns an object another call built lists it again under the producer lock. A refresh could not give the producer a complete index anyway: a body whose producer died between its rename and its epoch bump is invisible to every refresh and visible to a listing. The writer now updates the index under its lock, drains its own commit's inotify events and moves the object's last epoch seen past its own bump, so a process's own commits never cost it a refresh or a listing. That matters most when inotify is unavailable and every refresh lists. `take` builds and lists outside the registry lock, which stays a leaf.

#### The index was exempted from the producer limit (major)

The finding held. THREAD Session exempts the parent-key registry and nothing else of JITCache's own. Of the index, production allocates exactly the entries the writer adds, and those are now charged before the commit begins (main file section 9.4, step 8), at six bucket sizes each, until production ends. They are released with the rest of the production memory rather than when the entry goes: the index is shared, the entry stays to serve the process's importing VMs once production is over, and while the producing VM holds the lock nothing removes it. Entries that listings and refreshes create hold the bodies on disk and what other processes commit, which every importing VM needs whoever produces; they are not production memory. The finding's other option, no index in a Producer, assumed per-VM objects. In the shared design a Producer's index serves its process's workers.

Checking the charge rate showed that the old rule, four bucket sizes per entry, was not the bound it claimed. WTF doubles a table above 1024 buckets once it is half full, so while a rehash holds the old table beside the new one the two take six buckets per entry, and a table starts at eight buckets. The rule is now eight bucket sizes per table and six per entry (main file section 5.3).

#### Other changes in round 2

- `bodyVersion` returns 0 on a transient `EMFILE`, `ENFILE` or `ENOMEM` while the body is present. Section 7.2 used to say it never returns 0 for a present body. It now says that 0 means `openBody` would open nothing now, which is UCB R-INT-3's "0 when there is none"; the UCB lane takes it as a miss that stamps nothing.
- Section 12 let envelope reads run under the index lock, while the container's section 6.4 forbade it. Now only the inotify reads and the listings' calls run under it.
- The writer closes the temporary before the rename, so the close event is queued before the publish and the writer's own drain reads it.
- `delta`'s key vector is reserved at the table's size and charged 40 bytes per key, in place of "charged like the table".
- The container no longer gives an opened artifact maintenance users; maintenance lists the directory with descriptors of its own and never takes one.

### Round 3

Five findings, two blockers and three majors. All five held; for three of them the fix differs in detail from the one the finding suggested, as each record says.

#### The scoring read no longer needs a complete index (blocker)

A producing VM's index had to hold every body on disk for one reason: `readSavedSummaries` answered `Absent`, without a system call, for a key the index lacked. That shortcut cost a `ProducerLock` parameter on `ArtifactRegistry::take`, a second listing under the lock of an object another VM of the process had built, the completeness clause of II21, and the argument that only this listing finds a body whose producer died between its rename and its epoch bump. Round 2 had found defects in exactly this class of reasoning.

The read now opens the body by its name whatever the index holds. `ENOENT` is `Absent`, `EMFILE`, `ENFILE` and `ENOMEM` are `Unavailable`, any other error or failed check is `Invalid`, and success is `Found`: the same four outcomes, and the crashed producer's body is found with no listing. The cost is one failed `openat` for a key with no body, at a scoring that a full commit (create, stream, reread, rename) then normally follows, so the capture pause barely moves.

Nothing else depended on completeness. An import treats a key the index lacks as a miss, which THREAD Failures calls normal. The writer's index update had assumed that its VM's `start` drained the inotify queue; it now applies whatever events are queued, its own commit's and those of changes made before its VM took the lock, and records its own bump as seen only when the bump replaced the last epoch the object saw and no listing is pending. An object that was behind therefore refreshes at its next lookup, which also covers an object without an inotify descriptor. An overflow or a read error during that drain marks a listing pending instead of listing inside the capture pause. The scoring read updates an entry the index already has (its version on `Found`, its removal on `ENOENT`) and inserts none, so it adds no index memory the budget would have to account for.

#### Captures cannot nest, so the reentrancy guard went (blocker)

`delta.reentrant`, the capture-running mark and the twins-only capture probe guarded a state nothing reaches. A capture runs with JS paused and calls no host or JS function (THREAD Session: the API returns results "never callbacks"), finalizes no plan (THREAD Failures), and no lane's capture call walks the CodeBlocks with plan completion; the Image lane's twin compile uses `compileAndLinkWithoutFinalizing` outside `BaselineJITPlan::finalize`, and it runs at install, not at capture. The probe existed only to create the state the guard rejected. All three are gone, and T-DELTA lost its reentrancy clause. What remains is a debug-build assertion that no capture is in progress when the finalize capture or `delta` starts, which checks II14's consequence at no release cost.

#### The state's lifecycle moved to the task that creates states (major)

Task 2 defined the teardown hooks, and `VM::~VM` called them from task 2 on, while destroying a state runs `~ProducerLock`, `~OpenedArtifact` (task 5) and `~ArtifactWriter` (task 6). Checking the rest of the plan against the same rule found three more cases: C6, in task 5, expected invalid material, which only a configured VM can raise, and `start` lands in task 7; the writer's `rewriteSection`, in task 6, erased kept summaries, which task 9 introduces; and `start` created the Image lane's `Twins` object, whose class lands with that lane's task 11.

The finding offered two fixes: split the state's ownership by task, or keep the late members in a sub-object whose destructor lives in the store's and the writer's files. The second still needs those files at task 2's link. The first was taken in a simpler form. Only `start` creates a state and only `didFinalizeHeap` destroys one, so the constructor, the destructor and both teardown hooks moved into `JITCacheAPI.cpp` with `start`, in task 7, together with manifest entry M4; every type the state owns is defined by then, and no VM has a state before. The store's reads now return outcomes and know no VM, so C4 to C6 test them in task 5, and `VMState::bodyVersion` and `openBody` raise the faults. The kept-summary erase moved from the writer to the capture glue's `rewriteSectionForTesting`. `ValidatedBody` moved to a header and file of its own, defined in task 2: the UCB lane's registry and engine create and destroy pending imports, which hold one, so a lane task can link from task 2 on.

`Twins` is held in an image twin-check state that install step 19 creates at the VM's first stash, through a `std::unique_ptr` whose deleter the creating file supplies, so the state's destructor calls the deleter without needing the Image lane's type. SPEC-image.md R-INT-11 asks for the object "created when start configures the VM". What the lane needs from that is a lifetime covering every twin CB until VM destruction has deferred GC for good, and an object the first check creates and `willDestroyVM` destroys gives it.

#### The writer takes section sources (major)

`CommitSections` had one constructor, taking four lane types, and nothing the writer could read; the writer called the Image lane's size and write functions by name; and test C7, which lands before any lane's capture code, could not build one. The writer now takes an ordered list of sources, each a section kind, a size, and either a span or a function with an object that streams the bytes into a sink. The capture glue's `commitSectionsFor` builds the list from the lanes' outputs, and C7 builds one from spans and crafted streams.

Two details differ from the suggestion. The stream is a function pointer and an object pointer rather than a `WTF::Function`, so a commit allocates nothing besides the staging buffer and the glue can return the list by value. A source whose byte count differs from its size fails at a step of its own, `writer.section`, not at `writer.write`: it is a lane's broken size contract rather than an I/O error, and the diagnostic names the section and both counts, where the reread would have reported only a checksum or size mismatch. A refused charge for the staging buffer now comes back from the writer as `budget.limit`.

#### Test bodies and a lookup override for the lanes' self-tests (major)

SPEC-ucb.md U4 needs a pending import whose body asserts in its destructor, and U8 needs the engine's body lookup to return crafted bodies; `ValidatedBody` was final with no test constructor, and the lookups had no override point. In twins builds `ValidatedBody::createForTesting` now copies crafted sections into one buffer, each 8-byte aligned, and runs a callback from its destructor, and `VMState::setBodyLookupForTesting` makes `bodyVersion` and `openBody` answer from two functions. The factory takes spans rather than vectors, and the override answers alone instead of falling through to the store, once the activity test has passed, and moves no progress counter. Role, strictness and activity stay as `start` and the faults left them, so a lane test that needs several configurations runs once per configuration. Both are declared by task 0; the factory is defined in task 2 and the override in task 5, which section 18 now states for every integrator function another part calls.

### Five-part system review round 1

The first review of all five SPECs together filed two majors that reach this part: one against it, on the twins build's heap, and one against THREAD, whose measurement belongs to the integrator's bench.

#### The heap relocation domain in the twins build (major, premise refuted, gap fixed)

The finding: the twins profile is a Debug build, so ASan is on; bmalloc then sends every allocation to ASan's allocator; on x86_64 Linux that allocator's primary region sits at a fixed address and fills each size class in allocation order, so `&vm`, `jsEmptyString(vm)` and the VM's atoms repeat between producer and consumer. Every image's `VMAddress::SoftStackLimit` fixup would then give an equal pair in the heap domain, the runner's repetition would reproduce it, and T2 and T3 would fail in the default twins build, while H2 checked only the pool and the structure base.

The chain up to the allocator checks out in the code, and N17 now records it: Bun's `asanDefault` turns ASan on for Debug builds on Linux; the local WebKit recipe passes `ENABLE_SANITIZERS=address` and sets `USE_MIMALLOC` only without ASan; Bun's mimalloc leaves `malloc` to ASan; `Environment::computeShouldBmallocAllocateThroughSystemHeap` returns true once `isSanitizerEnabled` finds `__asan_init`; libpas's bmalloc heaps defer to the system heap (`pas_system_heap_is_enabled`), TZone heaps fall back to it (`determineTZoneMallocFallback`), and MarkedBlocks come through `FastMallocAlignedMemoryAllocator::tryAllocateAlignedMemory`. H2 did check only the two placed domains, so nothing in the harness tested the heap or the engine image.

The premise that ASan's space has a fixed base on x86_64 is false for the toolchain this project builds with. A ten-line C program compiled by the pinned clang 21.1.8 with `-fsanitize=address` allocated 90,000 bytes, 40 bytes and a 16 KiB-aligned 16 KiB block. In three fresh processes the first allocation landed at `0x7a9c3a3e0800`, `0x7714761e0800` and `0x7d75dc1e0800`, and the distances between the three allocations repeated exactly: the space is one mapping at a base the kernel chooses, and only the order inside it is deterministic. A position-dependent link (`-fno-pic -fno-pie -no-pie`) gave the same result, with the executable itself at one fixed address. So `&vm`, the cells and the atoms move together with that base, and the relocation clause does not fail deterministically on x86_64. SPEC-image-history.md revision 3 had inferred the same from the runtime's disassembly; this is the direct measurement.

ARM64 could not be measured here, and the sanitizer runtime chooses per architecture and per release whether its allocator space has a fixed base: older releases kept x86_64's at a fixed address, which is where the familiar `0x60...` heap addresses of old ASan reports came from, and the finding's premise describes those releases. A heap that stayed put would not pass unnoticed, since the clause would report the same pairs again in the repetition, but nothing would say why, and T2 and T3 could never pass on that architecture. So the harness now checks the domain and moves the heap itself where it must:

- `recordLayout` also writes the engine object's load bias, and the new `recordVMLayout` writes three heap probes, `&vm`, `jsEmptyString(vm)` and the `StringImpl` of `vm.propertyNames->length`, one for each kind of heap target an image names. The jsc shell calls it right after `VM::create`, and Bun's `configureVM` for the main thread's VM.
- Before its first twins sequence the runner calibrates: two fresh processes record their probes, and if one repeats, it calibrates again with `WebKitMallocForceEnabled=1`, which `computeShouldBmallocAllocateThroughSystemHeap` tests before the sanitizer and which hands the JSC heap to libpas, whose pages `mmap` places at addresses the kernel chooses. If a probe still repeats, the runner stops and names the heap domain. On x86_64 the first calibration passes and nothing changes.
- H2 checks all four domains over 50 sequences on each architecture, in the environment the calibration chose, and H1 drives the calibration's failure path through `--jitcache-test-fixed-heap-probes`.

The finding's other suggestions were weighed. `asan: false` would hand the JSC heap to Bun's debug mimalloc (`USE_MIMALLOC` without ASan), whose arenas, aligned to 64 KiB slices, are mapped at address hints that start at 2 TiB and are randomized only when `NDEBUG` or `MI_SECURE` is defined (`_mi_os_get_aligned_hint` in Bun's vendored `src/os.c`), so it would fix a heap that ASan moves; it would also take ASan out of every twins run, the `ics/` directory's included, which runs only in twins builds. Consuming a random number of chunks in every size class before `JSC::initialize` gives each target only as many positions as chunks consumed, so with dozens of heap fixups per body some pair would coincide in most runs, and the repetition would not clear it. The calibration keeps ASan wherever its space moves and gives up ASan's checks of the JSC heap only on a machine where it does not.

SPEC-image.md's domain table now names what moves the heap and points to the runner's check, and its R-INT-11 states the check and the fallback as requirements on this part; SPEC-image-history.md revision 7 records the change.

#### Exit sites induced across ConsumerProducer generations (major, against THREAD)

The finding: the CB lane's floor brings an imported body whose captured counter had crossed to its first DFG compile at its second invocation, when each property IC holds only what one invocation cached. At a site the producer saw with several shapes, that compile exits with `BadCache` until it is jettisoned, `CodeBlock::tallyFrequentExitSites` adds the site to the UCB's exit profile, the UCB lane counts every exit site toward richness, section 9.2's `beats` lets a recapture taken once the baseline CB is the replacement again win, and Bun's `delta` at exit commits it. Every later consumer's DFG then gives that site its slow status, and exit sites only accumulate. No test or bench ran more than one consumer generation.

Each step checks out in the code. `CodeBlock::jettison` calls `tallyFrequentExitSites` for a CB that is still its executable's replacement, for every reason except age and VM traps; `OSRExitBase::considerAddingAsFrequentExitSite` adds the site of every exit with a nonzero count; `DFG::ExitProfile::add` only appends; `GetByStatus::computeFor` returns `slowVersion()` for a site that exited. The UCB lane seeds the captured list into an imported UCB (`restoreFrequentExitSites`, SPEC-ucb.md section 8.3), so a recapture of an imported body carries every saved site and the new ones, and the sites accumulate as the finding says. Both levers, the richness rule and the floor together with the omission of IC cases, are THREAD's, and the lanes' provisional marks (SPEC-cb.md section 5.3, SPEC-ucb.md section 8.6) remain the narrowest choice THREAD allows.

The SPECs can still add the measurement the finding asked for. IB10 chains one Producer, `benchConsumerProducerGenerations` ConsumerProducer runs and one Consumer. Per generation it records the committed scores with the exit-site units apart, the field that decided each commit, the exit sites each generation added and how many of them sit at bytecodes whose captured IC listed two or more cases, and it compares the last consumer's first run with the producer's warmed run. The `capture` bench event gained the score fields and the deciding field. Bun's `delta` at exit stays: THREAD leaves the idle point to the host, and calling `delta` less often would slow the ratchet without settling it.

### Five-part system review round 2

Three majors reach this part: two against the CB and ICs lanes about the task plans, and one against the UCB lane whose fix uses a member of the VM state.

#### The lanes' twin checks waited on the glue that calls them (two majors, against SPEC-cb.md and SPEC-ics.md)

Task 8 waits in twins builds for each lane's twin check, because install steps 14 and 15 call `checkRestoredBaselineICs` and `verifyTwins`. The CB lane's task 6 held `verifyTwins` together with its corpus, which needs this glue and the runner, and the ICs lane's task 6 held `checkRestoredBaselineICs` and the snapshot function together with T2, a runner sequence; task 12 registers that snapshot function as `jitcacheICsSnapshot` and task 13 is the runner. Each of the two lane plans and this one waited on the other, so neither the twins build of the glue nor those lane tasks could land. Both findings hold.

The lanes split their tasks. CB task 6 is the twin check with its unit tests, and task 7 is the corpus; ICs task 6 is the twin API with a live C++ test, and task 7 holds T2 with the other runner tests. Section 18 now names the twin-check tasks task 8 waits for, adds the ICs lane's task 6 to task 12's dependencies for `jitcacheICsSnapshot`, and states the rule the plans follow: a lane task that an integrator task waits on depends on no integrator task from that one on. The Image lane's task 11 already kept its twin check apart from its runner tests, which wait in its task 12. The UCB lane's self-tests run through the `$vm` entry of task 12, once that task has landed, which SPEC-ucb.md section 17 now says; no UCB task waits on them.

#### Production for the UCB lane's direct-eval contexts (major against SPEC-ucb.md)

The UCB lane now digests a context only where something reads it. A direct eval's TDZ and private-name sets exist only during its request, so its record keeps the digest only in a VM whose production is active, the only VMs that capture (SPEC-ucb.md section 4.3). `VMState::productionActive()` moved from the integrator's own members into the lanes' interface, which task 2 defines, and section 5.2's production row names the effect. Section 9.1's eligibility notes that `captureRecord` answers only for a UCB whose context a capture can write.

### Five-part system review round 3

One major against this part and one against the Image lane that changes the harness's text. Both hold.

#### Bun's twins build called functions only an unexported header declared (major)

Bun's `JSCInitialize` called `placeholdersBeforeInitialize`, `releasePlaceholdersAfterInitialize` and `recordLayout`, and `JITCacheHost::configureVM` called `recordVMLayout`, all four declared only in `JITCacheTwinsHarness.h`, which section 3.1 did not export, while section 4.5 limited the one exported twins header to the four host functions. `scripts/build/deps/webkit.ts` gives Bun as JSC include roots only the build directory and the `Headers` and `PrivateHeaders` copies, so the `bun-twins` target could not compile, and task 14 and every Bun-hosted twins test waited on it. Bun's own `vendor/` include root does reach the WebKit sources through the `vendor/WebKit` symlink, but including a `jitcache/` header that way would bypass the rule SPEC-image.md section 15.4 states for every exported header.

The exported twins header is now `JITCacheTwinsHost.h`, named for who uses it: it declares, under `ENABLE(JITCACHE_TWINS)`, the four placement and layout functions with `JS_EXPORT_PRIVATE` and WTF and JSC types only, and the four host functions as before. Widening the old `JITCacheTwinsFunctions.h` would have worked too, but its name described only half of what it now holds, and nothing outside this part referred to it. `JITCacheTwinsHarness.h` keeps what only the jsc shell calls. The `--jitcache-test-fixed-heap-probes` flag reached `recordVMLayout` through no declared parameter; the function now takes `HeapProbes`, so the shell's call states it. Task 14 now ends by building `bun-debug` and `bun-twins`, which would have caught the defect.

#### Imports a process captured itself (major against SPEC-image.md)

A ConsumerProducer imports bodies it committed itself, because the writer updates the index its own lookups read (container sub-SPEC section 8.2, step 8), and the twin check's relocation clause then saw every pair equal; section 7.5 fails a run on any equal pool or structure-reservation pair. The Image lane now writes a capture-process token into the twins section and compares no relocation pair for a body its own process captured (SPEC-image.md section 17.2). Sections 4, 7.3 and 7.5 here say so; the runner itself does nothing differently.

## Drain after the thread-prep run (THREAD sha256 `b7d45aea…73fb1e`)

THREAD changed after the run, and the four lane sets were drained against it first. This drain applied THREAD's changes and the lanes' new text to this set and settled the run's one open finding against it. The native facts it adds were read in the code: the two paths that set `FTL::State::allocationFailed` (N9) and the local WebKit recipe's `CMAKE_POSITION_INDEPENDENT_CODE` (N15).

The three provisional marks are gone. P1 stood: THREAD Failures now makes the list of fault sites exhaustive and keeps Yarr, Bun's FFI thunks and stubs and WebAssembly native. Reading the FTL path showed that `FTL::LowerDFGToB3::compileCallFFIImpl` also sets `allocationFailed` when Bun's FFI invoke thunk cannot be allocated, inside the plan's lowering, so that plan raises `exec-alloc.ftl-plan` like any FTL plan short of executable memory, as SPEC-ucb.md R-INT-11 already said. P2 went the other way: THREAD keeps Bun's build flags and skips targets inside an object loaded at its link-time address, so no build is position-independent, `recordLayout` writes no engine line, H2 compares no engine bias, and only heap coincidences repeat a sequence. P3 stood as THREAD words it, read as the Image lane reads it: the image check also skips a body its producer compiled on a worklist thread, so a skip fails a run when that run and every earlier JITCache run of its sequence have concurrent JIT off.

Of THREAD's other changes, the entry point keeps the pinned signature, which every lane's call now uses. Strict is off by default in `Config`, the shell and Bun. The container splits each check into an integrity part both modes run (header digest, key, checksums, and the sizes and bounds those checksums need) and a structure part only strict adds (reserved bytes, tier, commit identifier, entry order and kinds, layout, required sections), which is what SPEC-ics.md R-INT-4 expects of the directory; the writer's reread still validates in full, because THREAD Capture has it validate what it publishes. `strict-off.js` became `default-mode.js`, which runs its corpus with strict on and again with the default. The capture glue calls the ICs lane before the CB lane, at scoring and at capture, and passes `hasPolymorphicSite` on, so P is zero for a body that carries no progress (II22, T-STAMP). IB3 counts the request point's work as the UCB lane divides it, and the digests kept for identity join the registry outside the producer limit.

Among the lanes' new requirements, UCB R-INT-3 has `bodyVersion` answer from the index alone, so the index holds a token per key instead of a commit identifier. A key gets a fresh token whenever the index learns of another body there: an event, a listing that finds a new key or a new inode (`d_ino`), or the writer's own commit, which records the temporary's inode. No lookup reads an envelope, and a listing keeps the token of a file it saw before, so a relisting after an inotify overflow, or every listing where inotify is unavailable, does not send the UCB lane back to bodies it already found not to fit. The test override answers tokens, kept apart from the commit identifiers bodies carry. The lanes' summary readers take the strict flag, so a rejected saved summary is invalid material only with strict on. The Image lane dropped `recordRebuildRefused`: install step 10 raises `budget.limit` when a ConsumerProducer's budget has refused, the old step 18 is gone, and `ChargeRefused` at capture raises `budget.limit` and no image fault. M1 takes the CB lane's six files, M2 exports only the integrator's three headers now that the UCB lane exports none, task 1 no longer waits for the UCB lane's task 0, and the ICs lane's M7 is gone.

The open finding (simplicity lens, blocker) held: the glue re-parsed the summaries a capture had just built to get the kept score. Each builder already returns the score of what it wrote. The CB lane's `score()` is a function of its summary bytes (SPEC-cb.md I8); `captureBaselineICs` returns the count it writes into the section header; and `liveRichness`, read in the same pause as `buildSections`, reads the arithmetic profiles and exit sites the feedback section holds, which neither JS nor the collector can change during a capture (SPEC-ucb.md F17). With strict off the readers now reject nothing, so the step parsed all of `ucb.feedback` in every capture pause for no check, and with strict on it imposed a check the UCB and ICs lanes chose not to make, the pattern round 1 removed from the writer. The kept score now comes from the builders, `ucb.capture-summary` and its failure row are gone, `scoreSections` scores only saved bodies, and T-STAMP checks that a kept score equals `scoreSections` of the committed file (II23).

IB10 measured whether the exit sites an import's early DFG compile induces accumulate across ConsumerProducer generations. THREAD's rule now keeps that compile away from bodies with a polymorphic property IC, and the ICs lane extends the rule to polymorphic call sites, so SPEC-cb.md no longer cites IB10. IB10 now checks that consumer-induced exit sites no longer accumulate and sizes what the rule leaves: the floor's residue in bodies that carry progress, and recaptures that drop the bit (SPEC-ics.md B6), with SPEC-ics.md B4's split of committed bodies that carry it. The `install` event records whether the import carried progress, the `capture` event the bit the glue passed, and IB10 reads the CB lane's `counterMode` and the ICs lane's records from committed bodies.

Three lane passages contradict this set or another lane and are left to the lanes: SPEC-ucb.md R-INT-4 has the container's validation guarantee all three UCB sections, which normal mode now trusts as SPEC-ics.md R-INT-4 says; SPEC-image.md section 15.4 still says the UCB lane exports the headers Bun calls; and SPEC-image.md R-INT-11 still asks for the `Twins` object at `start`, which section 5.1 creates at the first image check for the reason round 3 gave.

## Kept minors from the thread-prep run

- integrator batch 1.1 Bun consumer never flushes its bench report: fixed, new `flushBenchReport` called by both hosts at exit in every role (Bun's main VM survives exit unless `BUN_DESTRUCT_VM_ON_EXIT`), `Bun__JITCache__deltaAtExit` renamed `Bun__JITCache__atExit`.
- integrator batch 1.2 exec-alloc-faults.js expects a fault for every n: fixed, `null` accepted per n since `ExecutableAllocator::allocate` fuzzes Yarr's requests too, and each of the five steps must appear across the range.
- integrator batch 1.3 VMState and scoring types left open: fixed, task 7 completes `JITCacheVMState.h`'s private part, `JITCacheBench.h` declares `BenchField`, `BenchReport` and `RelinkTimer` (relink accumulator moved into the report), task 9 waits on task 10 so `BaselineJITPlan::finalize` is edited by 10, 9, 16 in turn.
- integrator batch 1.4 fallible steps without outcome: fixed, a refused kept-summary charge raises `budget.limit` (section 9.3 and the failure table), `start` checks `benchReportPath`, placement failures `exit(1)`.
- integrator batch 1.5 bench report loses events, support generation in native cost: fixed, the flush as in 1.1, and a `JITThunks` support-generation count flags `install` and `compile` events that generated per-VM support, which IB3 sets aside.
- integrator batch 1.6 integrator JS tests need twins-only functions: fixed, `integrator/` requires twins in harness section 7.4.
- integrator batch 1.7 second CRC-32C implementation: fixed, `crc32cExtend` calls `runtime/CachedTypes.cpp`'s `crc32c`, made external; Bun's `-march=armv8-a+crc` covers the ARM64 hardware path.
- integrator batch 1.8 Plan::bodiesDigest redundant: fixed, dropped; a confirmed plan's evictions already list every body's key, size and version.
- integrator batch 1.9 delta sorts keys nothing reads: fixed, phase 2 walks the candidate table; vector, charge and sort gone.
- integrator batch 1.10 oracle checks only the last run: fixed, the runner compares every run that configures JITCache, producers included, since SPEC-ics.md T9's one-Producer sequences have no other oracle.
- integrator batch 2.a `thresholdForOptimizeSoon` row (cross-set, from cb batch 1.9): fixed, the row left section 6.2; SPEC-cb.md section 10 makes the option free, since no captured threshold contains it in this version.
- integrator batch 2.b three IC limit rows (cross-set, from ics batch 1.1): fixed, `maxAccessVariantListSize`, `thresholdForUndesiredMegamorphicAccessVariantListSize` and `maxPolymorphicCallVariantListSize` joined section 6.2 as ICs rows, matching SPEC-ics.md section 10.2.
- integrator batch 2.1 relink timer during GC finalization: fixed, `CodeBlock::jettison` reinstalls in the End phase on whichever thread drives it; `RelinkTimer` measures only off GC work, `finalize` takes the accumulator at entry, and section 3.2's context names the GC case.
- integrator batch 2.2 DeferGC claimed on Bun's vm.Script route: fixed, section 10 names the deferral each engine route holds and says Bun's `compileSync` finalizes with none, which neither the fault entry point nor the hook's steps 1 to 4 need.
- integrator batch 2.3 II13 not enforced on the built bytes: fixed, `commitCapture` takes the saved score and stops without a commit when the built score no longer beats it after an intervening drain.
- integrator batch 2.4 record rebuilt after production ended: fixed, step 7 passes `RecordPolicy::Rebuild` only while production is active, the integrator's reading of Image R-INT-7.
- integrator batch 2.5 installation bound without warm support: already fixed by batch 1.5 (`supportGenerated` in the events, IB3 sets those bodies aside).
- integrator batch 2.6 index lock scope and an unconditional store: fixed in its current form; section 12 already matches the container and tokens replaced stored versions, and the `ENOENT` erase now happens only while the key still holds the token read before the `openat`.
- integrator batch 2.7 BenchField, RelinkTimer and the edit list: fixed, the types were declared by batch 1.3, and section 3.2 now lists the `BaselineJITPlan` constructor that sets `m_jitCacheMeasure`.
- integrator batch 2.8 worker twin reports and missing layout files: fixed, the runner checks every `run<i>.twins.<executionContextId>` file and passes only the layout files earlier runs wrote; section 4 already ended the run on an unreadable file.
- integrator batch 2.9 Plan::bodiesDigest redundant: already fixed by batch 1.8.
- integrator batch 2.10 a NotEligible image drops the key for that delta: fixed, phase 1 keeps each key's candidates in score order and phase 2 moves to the next one, as Image R-INT-6 treats `NotEligible` as an ineligible CB.
- integrator batch 3.1 BaselineJITPlan constructor missing from the edit table: fixed, section 3.2 already listed it (batch 2.7), and task 16 now names the constructor that sets `m_jitCacheMeasure`.
- integrator batch 3.2 finalize glue without GC deferral on Bun's vm.Script route: fixed, sections 9.7 and 10 already said so (batch 2.2); section 8.4 now says that with the LLInt off that route reaches the `compileSync` point's step 1 with no deferral, where loads and one registry lookup return `NotInstalled` for a UCB Bun's decode left without a record.
- integrator batch 3.3 RelinkTimer during GC finalization: already fixed by batch 2.1, the timer measures only when `!currentThreadIsDoingGCWork()`, so no collector thread writes `VMState`.
- integrator batch 3.4 integrator JS tests need twins-only functions: fixed, harness section 7.4 already required twins for `integrator/` (batch 1.6), and section 16's intro now runs a twins-only script in the twins build only.
- integrator batch 3.5 bench report never flushed at exit: already fixed by batch 1.1, Bun's exit hook and the jsc shell's loop end both call `flushBenchReport`.
- integrator batch 3.6 Bun-hosted runs turn --jitcache run options into BUN_JSC_ variables: fixed, harness section 7.7 maps the strict, producer-limit, bench-report and log options to `BUN_JITCACHE_*`, drops `--jitcache-delta-at-exit` and fails a script that passes any other `--jitcache` option, since `JSCInitialize` sends a rejected `BUN_JSC_` variable to `onCrash`; worker reports were fixed by batch 2.8.
- integrator batch 3.7 charges and opens without a stated outcome: already fixed by batch 1.4, the kept-summary charge comes before its entry with a stated refusal, the staging charge refusal returns `budget.limit` (container section 8.1), and `start` checks `benchReportPath`.
- integrator batch 3.8 Rebuild after production ended: fixed, step 7 already required active production (batch 2.4) and now names its budget, `producerContextIfActive()->budget()`.
- integrator batch 3.9 finalize hook cannot carry the compile event's timing: fixed, `JITCacheGlue.h` declares `didFinalizeBaselineCompilation(VM&, CodeBlock&, const BaselineCompileTiming*)`, task 9 passes null and task 16 fills the timing.
- integrator batch 3.10 anonymous namespaces merge in unified sources: fixed, section 3.1 now requires the `integrator` prefix or a per-file named namespace such as `ContainerInternal`, as `Heap.cpp`'s `HeapInternal`.
- integrator batch 4.1 whole-artifact deletion removes the header after a failed body unlink: fixed, maintenance section 4.5 stops before `header` when a body stays, returning `Failed` with `unlink-failed` and leaving a valid artifact, since a header-less `cache/` holding a body makes `start` reject every role (container section 1.3); a failed directory removal after the header is `rmdir-failed` with `Done`, the bodiless remnant being reusable; II20 and test M3 match.
- integrator batch 4.2 task 0's headers lack declarations: fixed, container section 5.2 declares `OpenedArtifact`, section 6.1 `BodyKeyHash` and `BodyKeyHashTraits` (byte fills through `std::bit_cast`, since `BodyKey` is trivially copyable and its `make` and `fromBytes` reject both values), section 4.5 `ContainerCheck`, `BodyLayout`, `validateBody` and the `BodyValidationStream` the writer's reread feeds; task 0 writes `JITCacheContainer.h`; section 6.4 declares T-START's `removeMainBuildIDForTesting`. The writer's constructor, `BenchField` and `RelinkTimer` were already declared.
- integrator batch 4.3 Rebuild after production ended: already fixed by batches 2.4 and 3.8.
- integrator batch 4.4 delta's key vector and sort: already fixed by batch 1.9; sections 5.3 and 17 (IB6) name only the candidate table.
- integrator batch 4.5 two production charges without a refusal point: fixed, a commit now charges the kept-summary entry it adds at step 8, beside the index entry, so a refusal writes nothing; the staging half was already fixed (container section 8.2, step 1), and section 9.3 and the failure table match.
- integrator batch 4.6 producer-limit.js depends on which charge is refused first: fixed, section 5.4 raises every refusal as `budget.limit`, with the lane's check in the detail when a lane's capture call fails while the budget has refused; the test's row says so.
- integrator batch 4.7 GC deferral claimed on Bun's vm.Script route: fixed, sections 8.4, 9.7 and 10 already said so (batches 2.2 and 3.2); II14 now limits the deferral to captures that write and says neither entry point asserts one.
- integrator batch 4.8 bench report lost when no VM is destroyed: already fixed by batch 1.1, the hosts' exit hooks flush in every role, which also covers `delta`'s `Faulted` path and section 16.3's main-VM report check.
- integrator batch 4.9 a version read overwrites a newer commit in the index: already fixed, `token` reads no file, the reads store nothing on success and erase only while the key keeps the token read before their `openat` (container section 6.4), and II8 has no stale-identifier clause.
- integrator batch 4.10 `commitSectionsFor` has no return type and `CommitSections` may borrow a dead array: fixed, `CommitSections` copies up to `numberOfSectionKinds` sources into its own array and borrows only their bytes and objects, and `commitSectionsFor` returns it by value (section 7.3).
- integrator batch 5.0 `RecordPolicy` restates whether the budget is null (cross-set, image batch 5.2): fixed, install step 7 passes `producerContextIfActive()`'s budget or null, which is non-null exactly in a ConsumerProducer whose production is active, as SPEC-image.md R-INT-7 now asks, and step 10 tests `budget && budget->hasRefused()`; the normative files no longer name `RecordPolicy`, and batch 2.4's line above keeps the name it had then.
- integrator batch 5.1 `compact` with ratio 0 deletes an artifact with no bodies: fixed, maintenance 4.2 step 7 deletes only at ratio 1 or when `targetBytes > 0` and the plan took every body, so ratio 0 only cleans, as THREAD Maintenance says; M2 and M3 cover both empty-artifact cases.
- integrator batch 5.2 no glue step releases production memory: fixed, `VMState::releaseEndedProductionMemory()` (a flag test while production is active) is called first by the install function, the finalize capture's step 2 and `delta` once its preconditions pass, and is no work in section 4.4's sense.
- integrator batch 5.3 tasks 9 and 10 both edit `BaselineJITPlan::finalize`: already fixed by batch 1.3, task 9 waits on task 10 and task 16 on task 9, and task 16 names `BaselineJITPlan`'s new members.
- integrator batch 5.4 undeclared interfaces: fixed. `JITCachePlatform.h`'s API (`ProcessFacts`, `processFacts`, `crc32cExtend`, the build-ID override) is declared in section 6.4 and written by task 0; the writer's twins hooks are in `ArtifactWriter`'s declaration (section 7.3) and the store's in container section 7.5; `ValidatedBody` declares its private constructor from a mapping and per-kind spans, with `OpenedArtifact` as friend, so it needs nothing from `JITCacheContainer.h`; the harness's new `request` event carries the UCB lane's request-point times, which IB3 joins with `install` by key. `BenchField`, `RelinkTimer` and the relink accumulator were already declared (batch 1.3).
- integrator batch 5.5 `BodyKey` has no shared hash traits: already fixed by batch 4.2, the index, the kept summaries and `delta`'s candidate table all use container section 6.1's `BodyKeyHash` and `BodyKeyHashTraits` from `ArtifactStore.h`.
- integrator batch 5.6 anonymous namespaces collide in a unified bundle: already fixed by batch 3.10, section 3.1 requires an `integrator` prefix or a per-file named namespace.
- integrator batch 5.7 fallible steps with no outcome: fixed. The bench path part was already fixed (`start` step 3, batch 1.4). `clean` now leaves a header-less remnant that holds an unknown file in place, which the next Producer reuses, and both maintenance calls check the parent and `cache/` before taking the lock, so a path without an artifact gives `NoArtifact` and creates no lock file (maintenance sections 2, 3 and 4.2; M1).
- integrator batch 5.8 the oracle's Off run can overwrite the compared description: fixed, an oracle run takes only the compared run's own options minus every `--jitcache` option and writes `<scratch>/oracle<j>.heap`, which no run of the sequence writes (harness 7.6; H1).
- integrator batch 5.9 the writer's streamed reread needs memory and a validator: already fixed for the validator (batch 4.2's `BodyValidationStream`); fixed for the memory claim, container section 8.1 now counts the stream's fixed stack object beside the staging buffer, neither growing with the body.
- integrator batch 5.10 `delta`'s sorted key vector: already fixed by batch 1.9, phase 2 walks the candidate table, with no vector, charge or sort left.
- integrator batch 6.1 `CommitSections` borrows a dead array: already fixed by batch 4.10, `CommitSections` copies its sources into its own array and `commitSectionsFor` returns it by value, so nothing dangles; dropping the class for a bare span was not needed.
- integrator batch 6.2 section 5.3 claims no exemption beyond the registry: fixed, section 5.3 now lists the two uncharged allocations that are not production memory, the scoring read's mapping and the bench report's buffer, each with its reason, and II4 points to that list.
- integrator batch 6.3 T-CHARGE ignores the first commit's staging buffer: fixed, T-CHARGE commits one key first so the staging buffer and the kept-summary table exist, then expects six bucket sizes of each table for a second key's commit.
- integrator batch 6.4 `delta` skips a key whose best candidate is `NotEligible`: already fixed by batch 2.10, phase 2 tries the key's next candidate.
- integrator batch 6.5 no cross-part rule against unified-source name collisions: fixed on this side, the new R-ALL-8 asks every part to prefix its file-local names or put them in a per-file named namespace; cross-set, since the UCB, Image and CB SPECs state no rule yet (SPEC-ics.md section 7 already prefixes `ics`).
- integrator batch 6.6 a ConsumerProducer's `delta` rereads its imported bodies' summaries: fixed in part, IB7 now counts those first scorings apart and names stamping the committed score in the envelope's reserved bytes as the first change to measure; the change itself waits for a measurement, and recording a kept summary at install was rejected as work in the bounded import window ahead of demand.
- integrator batch 6.7 the index lists and keeps every body in every process: fixed in part, IB2 now measures index bytes per body beside the bodies a run requests and names compact fingerprint entries as the first change to measure; the layout change waits for a measurement.
- integrator batch 6.8 install steps 14 and 15 dereference a null twin sink: fixed, both run their twin checks only while `twinReportSink()` is non-null, as R-ALL-2 says, and the harness table's UCB row says the same; the null case of `$vm.jitCacheUCBStatistics({ verifyRegistry: true })` is SPEC-ucb.md M4's, a cross-set item.
- integrator batch 6.9 unified-source helper names, again: fixed with 6.5.
- integrator batch 6.10 keyless UCBs' compilations keep charged records: cross-set, confirmed (a UCB without a record never gets one, SPEC-ucb.md section 5.4, and section 9.5 step 4 then stops), but releasing the record needs an Image-lane entry point or a recording condition that SPEC-image.md section 4.1 owns.
- integrator batch 7.1 plain-mode runs call twins-only helpers: fixed, R-ALL-4 and harness section 5.2 now require a script that calls a jsc function, `bun:jsc` export or `$vm` helper of harness sections 5.2 to 5.4 to declare `jitcache-requires: twins` or test that it exists; `integrator/` already requires twins (batch 1.6), the UCB lane marks its scripts (its batch 6.8), the ICs directory requires twins, and the image and CB sets call none of them.
- integrator batch 7.2 install steps 14 and 15 and the null sink: already fixed by batch 6.8.
- integrator batch 7.3 first import opens the body twice, and the unmap falls inside the import window: fixed. The double open went with the token index (`bodyVersion` makes no system call, T-LOOKUP). Outside twins builds the install function's reference is the last one (step 17 resolves the pending import, and a decoded UCB borrows nothing, SPEC-ucb.codec.md E7), so step 18 now drops it after closing the bound's total and times the drop as `bodyRelease`, which IB1 measures with the reading it undoes. Read as the integrator's reading of THREAD Session's "reading and faulting the artifact, which are measured separately"; IB1 names one `pread` into an owned buffer as the first change to measure if the release weighs.
- integrator batch 7.4 the installation bound cannot be measured per body: fixed. The `request` event (batch 5.4) carries the request point's parts and IB3 already counts everything THREAD now lists. Harness 9.1 now reads the thread CPU clock, a system call on Linux, only where a counted span begins or ends, one pair around the install function, and the `install` event's step breakdown reads `CLOCK_MONOTONIC`, which the vDSO serves (checked: `MonotonicTime::now` calls `clock_gettime(CLOCK_MONOTONIC)` on Linux).
- integrator batch 7.5 the same null-sink point: already fixed by batch 6.8.
- integrator batch 7.6 `ucb/` and `integrator/` call twins-only helpers in plain mode: fixed with 7.1.
- integrator batch 7.7 layout files of `Off` and `Maintenance` runs: already fixed by batch 2.8 (harness section 7.3 lists only files earlier runs wrote, and section 4 ends a run at `exit(1)` on an unreadable file).
- integrator batch 7.8 index cost, and four inotify events per commit: fixed in part. Body temporaries move to `cache/`, beside the header's, and the writer renames them into `bodies/` through `cacheFd` (container sections 1.1 and 8.2), so a commit raises one event in the watched directory and the mask drops `IN_CREATE` and `IN_CLOSE_WRITE` (section 6.3; II5 already says bodies enter `bodies/` only by that rename); `clean` and creation remove temporaries in `cache/` only, and C7 checks the single event. IB2 counts overflow relistings apart. Compact index entries stay a measured option (batch 6.7), and the start listing is THREAD's (UCB R-INT-3).
- integrator batch 7.9 unified-source names: already fixed by batch 6.5 (R-ALL-8).
- integrator batch 7.10 `RecordPolicy::Rebuild` after production ended: already fixed by batches 2.4 and 5.0 (step 7 passes the active production's budget or null, and step 10 raises only when that budget refused).
- integrator batch 8.a SPEC-image.md R-INT-12, whether a plan's compilation records (cross-set): fixed. Section 5.3 specifies `m_jitCacheRecordsImage` and `jitCacheRecordsImage()`, which the constructor sets on the VM thread when `producerContext(vm)` returns a context and the registry's `keyOf` returns a key; section 3.2's row lists them. Task 2 adds the member, false until task 7 computes it, so the Image lane's task 4 compiles against it without waiting on the registry; no compilation records before task 7 creates a state anyway. Checked that both constructor callers, `jitCompileAndSetHeuristics` and `JIT::compileSync`, run on the VM thread, and that `keyOf` takes only the registry's leaf lock.
- integrator batch 8.b the twin keep-alive is gone (cross-set, image batch 8): fixed. Section 5.1's lifetime paragraph, `willDestroyVM`'s description and harness 3.3's `ImageTwinCheckState` comment now say the `Twins` object holds no twin CB, each twin dying after its check without writing `didOptimize` (SPEC-image.md section 17.2, step 2). Section 5.1 also quoted R-INT-11's old "created when `start` configures the VM", which now reads "no later than the VM's first `Twins::checkImage`".
- integrator batch 8.c the image test hook's run flag (cross-set, image batch 8): fixed. Harness 5.1 adds `--jitcache-test-image-hook=<name>`, which `jscmain` passes to a new `setImageTestHookNamed` in `JITCacheTwinsHost.h` before `JSC::initialize`; harness 7.7 maps it to `BUN_JITCACHE_TEST_IMAGE_HOOK`, which Bun's `JSCInitialize` reads (section 14.2, the section 3.3 row, tasks 12 and 14). H1's own hook is `--jitcache-test-twin-entry=<kind>`, which writes a difference, a skip or a relocation coincidence in a named domain; H1 had named forced entries with no way to force them.
- integrator batch 8.d `verifyRegistry` takes `TwinReportSink*` (cross-set, UCB fixer): fixed in harness 3.2's row, which gives the first three verify calls `*twinReportSink()` while a report is open and `verifyRegistry` the pointer itself.
- integrator batch 8.1 install steps 14 and 15 ignore a null twin sink: already fixed by batch 6.8 (both run their checks only while `twinReportSink()` is non-null).
- integrator batch 8.2 the fallback listing per external commit, under the shared lock: fixed in part. Tokens already survive a listing for unchanged inodes and `bodyVersion` reads no envelope (batches 2.6 and 4.9, the drain). New: without inotify, or with a listing pending, a refresh lists at most once per `fallbackListingIntervalMilliseconds`, 1000 until IB2 tunes it (container 6.3, section 17, II16, IB2). C4 forces inotify off and sets the interval through two new `StoreTesting` hooks; it already forced inotify off with no hook to do it. Building the map outside `m_indexLock` was rejected: a swap would drop what the writer's commit and the drained events record meanwhile, and merging them would add the mechanism the interval makes unnecessary.
- integrator batch 8.3 `ics/` keeps concurrent JIT on, so no ICs import meets the image twin check: fixed. Harness 7.4 turns it off in `ics/` as in every other directory unless a run sets it, and a skip then fails those runs. No ICs test needs worker compilation, and T9's fuzz index is deterministic only with it off, since `s_numberOfExecutableAllocationFuzzChecks` counts every thread's allocations. P3 itself was already gone. SPEC-ics.md R-INT-7 item 4, which says these runs keep the default, is a cross-set item for the ICs lane.
- integrator batch 8.4 same point: fixed with 8.3.
- integrator batch 8.5 same point, with T9's determinism: fixed with 8.3.

## Harness pass for HARNESS.md (2026-10-07)

Per-body event counts. HARNESS.md has tests assert, per body, the bytecodes the LLInt executes, the compiles, OSR exits, jettisons and reoptimizations, and the engine counts none of them per body: its event log feeds only the per-bytecode profiler, which THREAD keeps off, and its exit counters live in optimized CBs that die after a jettison. Harness section 10 adds twins-only counters to each `UnlinkedCodeBlock` and increments them where the engine performs each event: in the LLInt at `traceExecution()`, the point the native tracer already uses, and at the three `call_direct_eval` labels the tracer misses; in the finalize hook and the `DFG::Plan::finalize` site the integrator already edits; in `handleExitCounts`; and in `CodeBlock::jettison`. Scripts read them with `jitcacheBodyEvents`, in any role. Benches read an end-of-run dump whose total keeps the counts of UCBs that died earlier, which Bun's code deletion after the entry script would otherwise lose.

The pin comparison. A previous attempt counted on the disassembler to name operations and thunks. The code says otherwise: operation labels exist only in Apple's internal ARM64E builds, x86_64's disassembler prints no label, and the baseline dump runs before the link tasks that write x86_64's calls to thunks. Harness section 11 therefore names addresses outside the engine, by code block, by the allocation headers `logJIT` prints, by the build's symbol table, and otherwise by the order in which a run first references an object. That last name is exact because both runs keep the collector off, so no address changes owner. The tool reads x86_64's thunk targets from the JIT dump, folds blinding, temporary-register constants and ARM64 branch compaction, exempts the `super_construct` blocks, and checks that both runs compile alike, since `BinarySwitch` seeds its shuffle from a process-wide counter. Blinding strikes one candidate in 64, so a twins-only switch forces it and H7 shows that every blinded form folds back. M3 now places `m_jitCacheState` last in `VM`, so no offset the code bakes moves.

Producer kills. The abort before the rename covered one of the states a commit leaves on disk. Harness section 12 replaces it with seven `SIGKILL` points, one for each state the writer's system calls leave, from before the temporary exists to after the rename, and `producer-kill.js` checks the outcome THREAD gives each: earlier bodies importable, the interrupted one absent or its earlier version intact, and a temporary that `clean` reports. `SIGKILL` runs no handler or destructor, as a killed producer would not.

## Native-fidelity review after the minors (2026-10-07)

- Finding 1, H6's log lines (blocker): held. `CodeBlock::jettison` prints `Jettisoning <CB> and counting reoptimization` before its two early returns for a CB already invalidated, and the DFG and FTL print their code while they link, before `DFG::Plan::finalize` can still return `CompilationFailed` or `CompilationInvalidated`, so those lines could outnumber the counts of section 10.1. H6 now counts lines printed where the counters sit: `Did invalidate` and `Did count reoptimization for` from `jettison`, and, with `--verboseOSR=true` added, the `result: CompilationSuccessful` line that each of the three deferred-compilation callbacks prints first in `compilationDidComplete`, right after the counter in `Plan::finalize`. N24 says where each `jettison` line prints, the new N25 covers the compile lines, and section 10.1 places the jettison count right before the `Did invalidate` print.
- Finding 2, the fallback of `jitcacheBodyEvents` to the UCB an `UnlinkedFunctionExecutable` holds (major): held. Both slots are private and share unions with the decoder and the cached offsets while `m_isCached` holds, and the public `unlinkedCodeBlockFor` decodes, generates or imports. Every test that calls the function reads a function after a call created its CB, so the fallback went, which also keeps the harness out of a header the UCB lane edits: the function returns `null` when there is no CB of the kind, as `jitcacheBodyKey` does, and the dump still reports a UCB that outlived its CBs.
- Finding 3, the dump's `total` (minor): held, and wider than reported. A dead UCB retires its counts only when a sweep destroys it, and the walk skips it before then. Blocks wait for a sweep, which the reviewer's `sweepSynchronously()` covers, but precise allocations, which hold up to eight cells of each UCB subspace, wait for the collection's epilogue, which the VM thread runs at its next stop point when the collector thread ended the collection. The dump now calls `stopIfNecessary()` and then `sweepSynchronously()` before the walk, and neither starts a collection. H6's dying-UCB case drops `--sweepSynchronously=true`, so it exercises the dump's own sweep.
- Finding 4, the support-generation count (minor): held. `JITThunks` is `final` and keeps its data members private, so `generateSlowPathHandler` could not reach `m_supportGenerations`. `JITThunks` gains a public `noteSupportGeneration()`, a relaxed `fetch_add` that all three sites call, and `supportGenerations()` is a relaxed load, enough because each reader compares two reads on its own thread.
- Finding 5, step 6's "dropped bits" (minor): held. A marker's first visit folds pending samples into `m_prediction` (`CodeBlock::visitChildren`), so a drain between the scoring and the build can add bits as well as drop them. Step 6 now says a drain intervened and names both directions; the `!beats(committed, saved)` test already covered both.

## Compaction (2026-10-07)

The set was shortened without changing what it requires. The four normative files stay, and no sub-SPEC was merged or split. Every section keeps its number and its subject, except that SPEC-integrator.md section 19 is gone, so no number names different content. A row that cites this file names the record that holds the argument the compaction cut.

| old (HEAD) | new |
|---|---|
| SPEC-integrator.md | SPEC-integrator.md, 183827 to 174224 bytes |
| SPEC-integrator.container.md | SPEC-integrator.container.md, 55884 to 50615 bytes |
| SPEC-integrator.harness.md | SPEC-integrator.harness.md, 83602 to 80786 bytes |
| SPEC-integrator.maintenance.md | SPEC-integrator.maintenance.md, 16092 to 15127 bytes |
| SPEC-integrator-history.md | unchanged above this section |
| SPEC-integrator.md preamble | preamble, with THREAD Execution's quote replaced by its citation; the three sub-SPEC descriptions moved into section 1, items 3, 4, 8 and 10 |
| 1, items 1 to 10 | 1, same items; 3, 4, 8 and 10 name their sub-SPEC and take the preamble's descriptions (item 3 gains the file names, the objects a process keeps for an opened artifact, the index refresh and the scoring read, so it lists what the container's preamble covered) |
| 1, "The lanes own everything" | 1, unchanged |
| 1, "In one paragraph" | 1, "In outline", shortened to the flow with the sections that state each step |
| 2, N1 to N14, N16, N18 to N25 | 2, unchanged |
| 2, N15 | 2, N15, the same facts as sub-bullets |
| 2, N17 | 2, N17; the measurement's details are cited from "Five-part system review round 1" |
| 3.1, table | 3.1, without the "exported to Bun" column, which a sentence after the table replaces; the `JITCacheTwinsHost.h` row points to 4.5 |
| 3.1, unified-sources paragraph | 3.1, one sentence that keeps the bundling fact R-ALL-8 cites from 3.1 and holds the integrator to R-ALL-8 |
| 3.1, private headers paragraph | 3.1, after the table |
| 3.2 | 3.2, unchanged |
| 3.3 | 3.3, which also takes 19's `CLAUDE.md` note |
| 4.1 | 4.1, unchanged |
| 4.2 | 4.2, wording tightened; step 10's sentence on the `Twins` object is left to harness 3.1 and 3.3 |
| 4.3, 4.4 | 4.3, 4.4, THREAD quotes replaced by citations |
| 4.5 | 4.5, as a list of the three exported headers and what each declares |
| 5.1, code and parts table | 5.1; the twin-check-state row cites harness 3.3 |
| 5.1, `jitCacheState` paragraph | 5.1, citing M3 for the declarations |
| 5.1, registry and identity digests uncharged | 5.3, list of uncharged allocations |
| 5.1, lifecycle paragraph | 5.1; the per-type task list is left to section 18 |
| 5.1, image twin-check state paragraph | harness 3.3; the task-11 reason is in "Round 3", "The state's lifecycle moved to the task that creates states" |
| 5.2 | 5.2; the index-entry charge joins the list of production memory |
| 5.3, code and `tryCharge` to recording paragraphs | 5.3, THREAD quote replaced by its citation |
| 5.3, charges, hash-table rule, index-entry charge | 5.3 |
| 5.3, uncharged index entries, registry exemption, scoring mapping and bench buffer | 5.3, one list of uncharged allocations |
| 5.4, entry points and their callers | 5.4, a table of site, caller and step |
| 5.4, raise bullets | 5.4; the reason every refusal is `budget.limit` is in batch 4.6 |
| 5.5 | 5.5; `willDestroyVM`'s `Twins` clause is left to harness 3.3 |
| 6.1 to 6.4 | 6.1 to 6.4, unchanged |
| 7.1 | 7.1, wording tightened |
| 7.2 | 7.2; `bodyVersion`'s refresh cadence is left to container 6.3, and the UCB lane paragraph is shortened |
| 7.3 | 7.3; the writer's opaque-bytes description is left to container 8.2 |
| 7.4 | 7.4, unchanged |
| 8.1 | 8.1, THREAD Restoration's quote replaced by its citation |
| 8.2 | 8.2; step 6 drops the gate's reason, step 7 is condensed |
| 8.3, 8.4 | 8.3, 8.4, unchanged |
| 9.1, 9.2 | 9.1, 9.2; quotes replaced by citations, and the strict rejection points to `SummaryRejection`'s comment |
| 9.3, table and kept-summary paragraphs | 9.3 |
| 9.3, cost of an absent body and index completeness | container 7.3; the cost argument is in "Round 3", "The scoring read no longer needs a complete index" |
| 9.3, why an unreadable body never scores as absent | 9.3, one sentence; the argument is in "Round 1", "A transient open failure scored a saved body as absent" |
| 9.4 to 9.7 | 9.4 to 9.7, quotes replaced by citations; 9.5's twins count sentence is shortened, harness 10.1 holding the rule |
| 10 | 10; the other fault sites and the exhaustive list are left to 5.4 |
| 11.1, `bodyVersion` and `openBody` rows | 7.2, cited in 11.1's introduction |
| 11.1, scoring-read rows | 9.3's table, and container 7's table for the `ENOENT` row's index-entry erase, both cited in 11.1's introduction |
| 11.1, other rows and closing sentences | 11.1, unchanged |
| 11.2 | 11.2; the last paragraph is left to sections 16 and 17 |
| 12 | 12; the leaf locks' system calls are left to container 5.1 and 6.4 |
| 13 | 13, unchanged |
| 14, introduction | 14, THREAD Session's quote replaced by its citation |
| 14.1 | 14.1; the end-of-run actions are a numbered list |
| 14.2 | 14.2; `configureVM`'s bullet is split into sub-bullets |
| 15, 15.1, 15.2 | 15, 15.1, 15.2, unchanged |
| 16, introduction | 16; the oracle's description is left to harness 7.6 |
| 16.1 to 16.3 | 16.1 to 16.3, unchanged |
| 17, introduction | 17, THREAD quotes replaced by citations |
| 17, IB1 to IB11 and the parameter table | 17; IB10's restatement of THREAD Restoration keeps its condition and drops the clause on the cases one invocation rebuilt |
| 18 | 18; the introduction's lifecycle sentence is condensed |
| 19, `CLAUDE.md` note | 3.3 |
| 19, file names | container 1.1; the argument is in "Draft 1", "File names" |
| 19, one opened artifact per process | container 5.1; the argument is in "Round 2", "Shared opened artifacts did not refresh by role" |
| 19, no index holds every body | container 5.1 and 7.3; the argument is in "Round 3" |
| 19, no reentrancy guard | 9.7; the argument is in "Round 3", "Captures cannot nest" |
| 19, the twins heap moves on x86_64 | N17, harness 4, 7.3 and H2; the argument is in "Five-part system review round 1" |
| container preamble | container preamble; its content list is left to SPEC-integrator.md section 1 |
| container 1.1 | 1.1; the body name's hash reason is in "Draft 1", "File names" |
| container 1.2, 1.3 | 1.2, 1.3, unchanged |
| container 2 | 2, THREAD quote replaced by its citation |
| container 3.1 to 3.3 | 3.1 to 3.3, unchanged |
| container 4, introduction | 4, THREAD quote replaced by its citation |
| container 4.1 | 4.1; why the identifier is random is in "Draft 1", "Reading bodies" |
| container 4.2, 4.3 | 4.2, 4.3, unchanged |
| container 4.4 | 4.4; the trust sentence is left to 7.4 |
| container 4.5 | 4.5; the modes' reasons are cut, and the alignment sentence is left to SPEC-integrator.md section 7.1 |
| container 5.1 | 5.1; the per-VM alternative's costs are in "Round 2" |
| container 5.2 | 5.2; the build and destruction steps are left to the declaration's comments |
| container 6.1 | 6.1, unchanged |
| container 6.2 | 6.2; the `dup` offset argument is in "Round 2", "Every listing after the first read an empty directory" |
| container 6.3 | 6.3; the writer's own-commit handling is left to 8.2, the cadence to II16, and the interval and failed-listing reasons are in batch 8.2 and "Round 2" |
| container 6.4 | 6.4, unchanged |
| container 7, introduction and table | 7, unchanged |
| container 7.1 | 7.1; the refresh's system calls are left to 6.3 |
| container 7.2 | 7.2, THREAD quote replaced by its citation; its pointer to every case names section 7's table, which now holds the cases section 9 listed |
| container 7.3, steps | 7.3 |
| container 7.3, closing paragraph | SPEC-integrator.md 9.3 (outcomes) and 5.3 (the mapping's exemption); the cost is in "Round 3" |
| container 7.4 | 7.4, tightened |
| container 7.5 | 7.5, unchanged |
| container 8.1, 8.2 | 8.1, 8.2; quotes replaced by citations, and step 4's reason for `writer.section` is in "Round 3", "The writer takes section sources" |
| container 8.3 | 8.3, unchanged |
| container 9, `open` and `readSavedSummaries` rows | 7's table, with SPEC-integrator.md 7.2 and 9.3; one row cites them |
| container 9, other rows | 9, unchanged |
| container 10 | 10, unchanged |
| harness preamble | harness preamble; THREAD's quote and the content list are left to SPEC-integrator.md section 1 |
| harness 1 | 1; THREAD quote replaced by its citation, and the relocation skip is left to section 4 |
| harness 2 | 2; the `start.config` sentence is left to SPEC-integrator.md 4.2, step 3 |
| harness 3.1 | 3.1, R-INT-10's quote replaced by its citation |
| harness 3.2 | 3.2, unchanged |
| harness 3.3 | 3.3, which also takes SPEC-integrator.md 5.1's twin-check-state paragraph |
| harness 4 | 4; R-INT-11's quote is paraphrased |
| harness 5.1, 5.3, 5.4 | 5.1, 5.3, 5.4, unchanged |
| harness 5.2 | 5.2; the plain-builds rule is left to SPEC-integrator.md R-ALL-4 |
| harness 6 | 6, THREAD quote replaced by its citation |
| harness 6.1, 6.4 | 6.1, 6.4, unchanged |
| harness 6.2, 6.3 | 6.2, 6.3; the reasons for leaving promise reactions and registry-only values unwalked are in "Round 1", "The oracle had no way to compare the reachable heap" |
| harness 7.1, 7.2, 7.5, 7.8 | 7.1, 7.2, 7.5, 7.8, unchanged |
| harness 7.3 | 7.3; the self-captured import sentence is left to 4 and 7.5 |
| harness 7.4 | 7.4; why `ics/` needs no concurrency is in batch 8.3 |
| harness 7.6 | 7.6; the producer-oracle reason is in batch 1.10, and the script rules are left to SPEC-integrator.md R-ALL-4 |
| harness 7.7 | 7.7; its second `delta` sentence and the Bun global object's reason are left to its first sentence and 5.3 |
| harness 8.1, 8.3 | 8.1, 8.3, unchanged |
| harness 8.2 | 8.2; why options cannot be set before `JSC::initialize` is in "Round 1", "`testjitcache` set options before they existed" |
| harness 9.1 | 9.1; the budget exemption is left to SPEC-integrator.md 5.3 |
| harness 9.2 | 9.2, unchanged |
| harness 9.3 | 9.3, quotes replaced by citations |
| harness 10 | 10, introduction condensed |
| harness 10.1 to 10.3 | 10.1 to 10.3, unchanged |
| harness 11 | 11; the disassembler facts are left to N18 |
| harness 11.1 to 11.5 | 11.1 to 11.5, unchanged |
| harness 12 | 12, THREAD quotes replaced by citations |
| harness 13 | 13, unchanged |
| maintenance preamble, 2, 3, 4.1, 4.2 | the same sections, THREAD quotes replaced by citations |
| maintenance 4.4 | 4.4; the closing "without it" sentence is left to the `Answer::Ask` and `Answer::No` bullets |
| maintenance 1, 4.3, 4.5, 5, 6, 7 | the same sections, unchanged |

## Report drain (2026-10-07)

- spec-integrator-commit-score-sentence-stale: fixed. Section 7.3 said the published score was "computed from the summary bytes the glue handed it", the design the drain after the thread-prep run removed; it now cites section 9.4, step 6, where the score comes from what the builders return and II23 forbids reading bytes back.
- spec-integrator-glue-header-lists-debugger-hook: fixed. Section 3.1's `JITCacheGlue.h` row no longer lists a debugger hook, since `didAttachDebugger` is declared in `JITCacheFaults.h` (sections 3.1 and 5.4), and it now names the twins-build image-check hook that harness section 3.3 declares there.
- spec-integrator-harness-bun-jsc-export-check-vs-r-all-4: fixed. Harness section 5.3 required an existence test before every call, stricter than R-ALL-4; it now defers to R-ALL-4 as section 5.2 already does. Section 16.3's Bun test calls none of the exports, so no test outside the runner needs its own rule.
- spec-integrator-maintenance-epoch-bump-on-lock: fixed. Maintenance section 2 read as a bump at every lock acquisition; it now bumps "when sections 3 and 4.5 say so", that is after removals in `clean` and once after an applied plan, as container section 2 states.
- spec-integrator-openbody-enoent-erase-unconditional: fixed. Section 7.2's `openBody` bullet now erases the key on `ENOENT` only while it still holds the token read before the `openat`, as container sections 6.4 and 7 require for II8.

## Walkthrough reports

- spec-integrator-pointers-after-compaction item 1: fixed. Section 9.4's step 2 and section 11.1's `NotEligible` row said the key is skipped, while Image R-INT-6 and section 9.6 treat the CB as ineligible and let `delta` try the key's next candidate; both now say the CB counts as ineligible.
- spec-integrator-pointers-after-compaction item 2: fixed. Section 8.2's step 18, section 12, IB1, IB3 (twice) and harness section 9.3 cited THREAD Session, and section 7.1 THREAD Capture, for rules that sit in THREAD's untitled opening: the installation bound and what it measures apart, the no-threads rule and the format's room for more tiers. They now cite "THREAD's opening", SPEC-ucb.md's term; the set's other THREAD pointers were checked and hold.
- spec-integrator-pointers-after-compaction item 3: fixed. Section 7.3's `commitSectionsFor` table took the ICs buffer from step 5 of section 9.4, the CB lane's capture; step 4 builds it.
- spec-integrator-pointers-after-compaction item 4: confirmed. The coordinator's "section 9.2" in T-SCORE is right: section 9.2 defines `SummaryRejection`'s parts and checks and makes a rejected summary invalid material, while section 9.3 raises only container checks.
- spec-integrator-pointers-after-compaction item 5: fixed. SPEC-cb.md section 7 fails `scoreLive` at SC2, eligibility, as well as at SC3, pairing, so sections 9.2 and 9.6 now name both guards.
- spec-integrator-pointers-after-compaction item 6: fixed. Section 9.4's step 6 cited F17, which covers exit sites only; it now also cites profiles.md "Table", whose writers of the arithmetic profiles all run on the thread that runs JS.
- spec-integrator-pointers-after-compaction item 7: fixed. Section 10's comment now cites SPEC-ucb.md F24 beside N3, which says only that a null `BaselineJITCode` fails the plan; the code agrees that `JIT::link` returns null only after `didFailToAllocate()`.
- spec-integrator-pointers-after-compaction item 8: fixed. Harness section 7.6 called SPEC-ics.md T6 a last-run comparison; T6 compares every run that configures JITCache.
- spec-integrator-runner-unexpected-faults: fixed as the user decided. The runner gives every run with JITCache `--jitcache-log`, or `BUN_JITCACHE_LOG=1` under Bun, whose at-exit hook now logs the final status in every role (section 14.2). Harness section 7.5 fails a run that wrote no final status, unless it is expected to end by a signal; a run whose `activityFault` or `productionFault` names a step that no `jitcache-expect-fault` directive of the run declares; and a Consumer or ConsumerProducer run whose statuses add up to no install, unless `jitcache-expect-no-install` names it. Both fault fields are read, because `firstFault` alone would hide a fault that follows a declared one. A directive permits its step, a run may carry several, and a run that reports no fault passes, which the fuzz tests need, since their step depends on n and may be none. R-ALL-4 asks every part to declare its faults and its runs with nothing to install; `consumer-producer.js`, `exec-alloc-faults.js`, `producer-limit.js`, `writer-faults.js` and `must-match.js` declare theirs, `producer-kill.js` takes n of at least 2 so that its consumer has a body to install, and H1 tests both checks.
- spec-integrator-exec-alloc-test-cannot-see-captured-effects: fixed in part. Twins compare state the engine cannot recompute with its capture record, so they pass on captured failure effects, and the row's sentence saying they showed none was captured is gone. After each fuzzed `Producer --jitcache-delta-at-exit` run, the following Consumer, whose activity is on, calls the script's functions and reads their committed bodies with `jitcacheBodyKey` and `jitcacheReadSection`; a Producer that raised the fault has turned its own lookups off. No `ucb.feedback` may hold a deferred LLInt counter and no `cb.state` a carried, deferred baseline counter. The bodies stay below the reoptimization count at which `adjustedCounterValue` clips and keep every site monomorphic, since a `NotCarried` section records no counter (SPEC-cb.md I16). The `cb.state` check catches a DFG plan whose fault was missed. A missed baseline-plan fault defers its body's LLInt counter, so that body never compiles again and its feedback reaches a capture only through another live baseline CB of the body; the LLInt check holds SPEC-ucb.md section 8.5's invariant, and the baseline-plan site, like the FTL, IC and MathIC sites, still relies on its step appearing across the range.
- thread-capture-tie-break-defeats-polymorphic-rule, the coordinator's edits in this set: checked. `CaptureScore`'s field order, the comment above `beats`, both score sources, section 9.4's step 6, section 9.6's ranking, II13 and II23 follow THREAD Capture's new order. Section 9.2's reason for calling the ICs lane first and II22 described only the zeroed progress and now also name the withheld mark; T-SCORE adds the reversed pair, which the candidate loses, and T-STAMP checks `counterWithheld` in both kept scores. SPEC-cb.md R-INT-5 still has progress break the tie right after the IC-site count, a cross-set item.

## Options table

- Sections 6.1 and 6.2 no longer list options: docs/JitCache/options.md holds the must-match, fixed and free options of the five sets, and its fixed table keeps section 6.2's row order, which `checkFixedOptions()` walks. Sections 6.1 to 6.3 keep the header, the check and the build-time table; N12, section 6.4, section 15.2 and harness section 11.2 cite options.md. No option changed class, value or type.

## Runner directives by sequence

A directive's run index first applied to every sequence of a script, so `context-miss.js`'s waiver for its reverse sequence also dropped the install check in the two sequences that do install, and `invalid-material.js` and `start-line.js` could not tell their cases apart. The runner now passes each run its sequence index as `arguments[3]`, and a run index may be written `<sequence>:<run>`. `start-line.js` became two `Producer; Consumer` sequences, since its two consumers shared one sequence and the index could not separate them. The same grammar carries `jitcache-expect-twin`, which makes a declared twin report required and harmless, because SPEC-image.md T3's hook forces relocation pairs that section 7.5 otherwise fails.
