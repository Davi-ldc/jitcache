# SPEC-integrator.harness: test builds and bench reports

Part of [SPEC-integrator.md](SPEC-integrator.md), which indexes it and lists what this file covers (its section 1): the test builds THREAD Verification relies on and the tools every part's tests run on. [SPEC-integrator-history.md](SPEC-integrator-history.md) records the path to its non-obvious decisions. Everything here exists only in `ENABLE(JITCACHE_TWINS)` builds except the bench report, the runner's plain mode and the pin comparison of section 11, whose forced blinding is again twins-only. The facts only this file uses, N16 to N27, sit in the sections they serve; each was verified by reading the code at this pin, except the measurements in N17, N26 and N27, which the history records.

## 1. The twins build

THREAD Execution's test builds are those with `ENABLE(JITCACHE_TWINS)`, which the CMake option `ENABLE_JITCACHE_TWINS` switches on, off by default (SPEC-integrator.md section 14, M8).

A twins build compiles and links with Bun's build flags, as every build does (SPEC-image.md R-INT-11), except that Bun's own sources do not get `BUN_DYNAMIC_JS_LOAD_PATH` and its Rust gets `--cfg=jitcache_twins` (below). The engine therefore sits at its link-time address in every process of the build, in the jsc shell and in Bun alike (SPEC-integrator.md N15), which section 4 relies on. The UCB lane's Bun-hosted tests run with twins on too (SPEC-ucb.md section 13), so the profile serves both hosts. ([history](SPEC-integrator-history.md#twins-builds-keep-buns-build-flags-and-the-runner-moves-the-heap)) In `~/bun`:

- `scripts/build/config.ts`: `PartialConfig` and the resolved `Config` gain `jitcacheTwins`, false unless the profile sets it.
- `scripts/build/profiles.ts`: `"debug-local-twins": { buildType: "Debug", webkit: "local", jitcacheTwins: true }`.
- `scripts/build/deps/webkit.ts`, when `cfg.jitcacheTwins` holds: `ENABLE_JITCACHE_TWINS: "ON"` and the nested build's targets `jsc` and `testjitcache`. Its compile flags and `CMAKE_POSITION_INDEPENDENT_CODE` stay as every local build has them, and the build-id link flag comes from the rule of SPEC-integrator.md section 11.2.
- `scripts/build/rust.ts`: `--check-cfg=cfg(jitcache_twins)` in every build and `--cfg=jitcache_twins` when `cfg.jitcacheTwins` holds, beside its other custom cfgs, so that Bun declares the four flags of section 5.1 it takes in twins builds only (SPEC-integrator.md section 11.2).
- `scripts/build/flags.ts`: the entry that defines `BUN_DYNAMIC_JS_LOAD_PATH` also requires `!c.jitcacheTwins`. A twins Bun then embeds the internal modules' text and the builtins section's digest table as a release build does, so its runs take each internal module's digest from its provider (SPEC-ucb.md section 7.2.6) and check it through SPEC-ucb.md T7.

In this repository's `build.ts`, two targets join the table: `twins` (profile `debug-local-twins`, ninja target `WebKit`, which builds `jsc` and `testjitcache`) and `bun-twins` (profile `debug-local-twins`, Bun's default targets, which build the Bun executable against that WebKit). Both share the profile's build directory, as `debug` and `bun-debug` do. A third target, `pin`, builds the pinned engine for the comparison of section 11 (section 11.1).

## 2. The twin report

`TwinReport.h`:

```cpp
#if ENABLE(JITCACHE_TWINS)
namespace JSC::JITCache {

enum class TwinPart : uint8_t { UCB, Image, CB, ICs, Integrator };
enum class RelocationDomain : uint8_t { EngineImage, ExecutablePool, StructureReservation, Heap };

class TwinReport {
    WTF_MAKE_NONCOPYABLE(TwinReport);
    WTF_MAKE_TZONE_ALLOCATED(TwinReport);
public:
    static std::unique_ptr<TwinReport> open(const String& path);   // appends; null when the file cannot be opened
    void difference(TwinPart, ASCIILiteral check, String detail);
    void skip(TwinPart, ASCIILiteral check, String reason);
    void relocationCoincidence(RelocationDomain, String detail);
    unsigned differences() const;
    unsigned skips() const;
    unsigned coincidences() const;
    ~TwinReport();   // closes the file

private:
    explicit TwinReport(int fd);

    const int m_fd;   // opened for appending; each line goes out in one write, with no buffer of its own
    unsigned m_differences { 0 };
    unsigned m_skips { 0 };
    unsigned m_coincidences { 0 };
};

using TwinReportSink = TwinReport;   // the name SPEC-ucb.md uses

}
#endif
```

Every part reports through it: SPEC-ucb.md's verify functions, SPEC-cb.md's `verifyTwins`, SPEC-image.md's `Twins::checkImage` (differences, skips and the relocation clause's equal pairs, each with its domain) and the integrator for SPEC-ics.md's mismatches. Each call appends one JSON line to the file and flushes it, so a later crash keeps it: `{"kind":"difference","part":"image","check":"…","detail":"…"}`, `{"kind":"skip","part":"image","check":"…","reason":"…"}` or `{"kind":"coincidence","domain":"engine-image","detail":"…"}`, with the domains spelled `engine-image`, `executable-pool`, `structure-reservation` and `heap`. Details name the body by its key in hex and the CB by `CodeBlock::dump`. The report is used on the VM thread only.

## 3. Twin checks in a VM

Each part's twin check runs here:

| check | where | caller |
|---|---|---|
| SPEC-ucb.md `verifyImport`, `verifyMatched`, `verifySuppliedDigest`, `verifyRegistry` | inside the UCB engine and `$vm.jitCacheUCBStatistics()` | the UCB lane: the first three with `*twinReportSink()` while a report is open (SPEC-integrator.md R-ALL-2), and `verifyRegistry` with `twinReportSink()` itself, which counts violations without a report and reports them only through a non-null sink |
| SPEC-ics.md `checkRestoredBaselineICs` | install step 14, right after `attachPropertyICState` | the integrator; each `TwinMismatch` becomes `difference(ICs, mismatch.field, detail)`, the detail giving the site, the index, the expected and the actual value |
| SPEC-cb.md `verifyTwins` | install step 15, between `finishCounter` and `installCode` | the integrator |
| SPEC-image.md `Twins::checkImage` | the end of `prepareForExecutionImpl`, after `installCode` and before JS runs | the integrator (below) |

`JITCacheInstall.cpp` defines, in twins builds, the VM's image twin-check state and the deleter the `VMState` holds it with (SPEC-integrator.md section 4.1):

```cpp
struct PendingImageTwinCheck { CodeBlock* codeBlock; Ref<BaselineJITCode> code; Ref<ValidatedBody> body; ImageSectionsView view; };
struct ImageTwinCheckState {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(ImageTwinCheckState);
    Twins twins;                                   // SPEC-image.md section 11.3
    std::optional<PendingImageTwinCheck> pending;  // at most one
};
```

Install step 18 (SPEC-integrator.md section 7.2) creates the state at the VM's first stash, with `{ new ImageTwinCheckState, deleter }` where the deleter is a captureless lambda that deletes it, and stores the check in `pending`. The `VMState`'s destructor calls that deleter, so it needs no Image-lane type, and an empty holder calls nothing. Created at the first stash and destroyed by `willDestroyVM`, the `Twins` object exists no later than the VM's first `Twins::checkImage` and dies during VM destruction, as SPEC-image.md R-INT-11 asks.

`prepareForExecutionImpl` consumes the pending check before it returns, and no JS runs between the install and that point. The body's reference keeps the parsed view's bytes alive until the check returns (SPEC-image.md R-INT-11).

`JITCacheGlue.h` declares, in twins builds, `void didFinishPrepareForExecution(VM&, CodeBlock&, JSScope*)`, which `prepareForExecutionImpl` calls last (SPEC-integrator.md section 7.1). When `pending` holds this CB and a report is open, it calls `state.twins.checkImage(vm, codeBlock, scope, *code, view, *twinReportSink())`. When it holds another CB, the install ran in `JIT::compileSync` outside `prepareForExecutionImpl` and no scope exists, so it reports `skip(Image, "no-scope", …)`. Either way it clears `pending`, which drops the body; a VM with no state, or whose state holds nothing pending, returns at once. It runs on the VM thread inside `prepareForExecutionImpl`'s `DeferGCForAWhile`; `checkImage` opens its own deferral for the twin CB it creates (SPEC-image.md section 11.3).

## 4. Address-space placement

- N16. `g_jscConfig.startExecutableMemory`, `endExecutableMemory`, `startOfStructureHeap` and `sizeOfStructureHeap` (`runtime/JSCConfig.h`) hold the two process-wide reservations once `JSC::initialize` has made them.
- N17. The twins profile is a Debug build, and Bun's config turns ASan on for every Debug build on Linux (`asanDefault` in `scripts/build/config.ts`). With ASan the local WebKit recipe passes `ENABLE_SANITIZERS=address` and sets `USE_MIMALLOC` only without it (`scripts/build/deps/webkit.ts`), and Bun's mimalloc leaves `malloc` to ASan (`override` in `scripts/build/deps/mimalloc.ts`). bmalloc then sends every allocation to the system heap, ASan's allocator, because `isSanitizerEnabled` finds `__asan_init` (`Environment::computeShouldBmallocAllocateThroughSystemHeap`): libpas's bmalloc heaps defer to it (`pas_system_heap_is_enabled`), TZone heaps fall back to it (`determineTZoneMallocFallback`), and `FastMallocAlignedMemoryAllocator::tryAllocateAlignedMemory` takes MarkedBlocks from it through `tryFastCompactAlignedMalloc`. The VM, its cells and atoms and every UCB's vectors therefore live in the sanitizer allocator's space. With the toolchain's clang 21.1.8 on x86_64 that space lies at a different base in every process, position-independent or not, a measurement the history records; ARM64 was not measured. ([history](SPEC-integrator-history.md#twins-builds-keep-buns-build-flags-and-the-runner-moves-the-heap)) `computeShouldBmallocAllocateThroughSystemHeap` tests the environment variable `WebKitMallocForceEnabled` before the sanitizer, and with it set bmalloc uses libpas, which maps its pages with `mmap` at addresses the kernel chooses (`pas_page_malloc.c`).

SPEC-image.md R-INT-11 has the runner move the relocation domains the image check compares, and THREAD Verification's relocation requirement skips the engine image, loaded at its link-time address under Bun's build flags (section 1), and every target of a body the importing process captured itself (SPEC-image.md section 11.4), which leaves three domains. Placement moves two: the importing process's executable pool to a range disjoint from the capturing process's, and its structure reservation to another base, both settled before its `JSC::initialize` makes them. Address randomization, which stays on, moves the heap with the base of the space its allocator maps, ASan's in the twins build, whose space moves on x86_64 (N17). Under QEMU user mode no guest mapping moves by itself, and the runner moves the heap with each process's guest stack size instead (N26, section 7.9). Before its first twins sequence the runner checks that space on the machine it runs on and takes the heap off it where it stays put (section 7.3), and H2 checks the three domains on each architecture. ([history](SPEC-integrator-history.md#imports-a-process-captured-itself)) Both hosts call these functions, so `JITCacheTwinsHost.h` (SPEC-integrator.md section 3.5) declares them in `namespace JSC::JITCache`, under `ENABLE(JITCACHE_TWINS)`:

```cpp
enum class HeapProbes : bool { Live, Fixed };
JS_EXPORT_PRIVATE void placeholdersBeforeInitialize(const Vector<String>& layoutPaths);          // before JSC::initialize
JS_EXPORT_PRIVATE void releasePlaceholdersAfterInitialize();                                     // right after JSC::initialize
JS_EXPORT_PRIVATE void recordLayout(const String& path);                                         // after JSC::initialize
JS_EXPORT_PRIVATE void recordVMLayout(VM&, const String& path, HeapProbes = HeapProbes::Live);  // once the host's main VM exists
```

`recordLayout` writes two lines, in hex: `pool <start> <end>` from `g_jscConfig.startExecutableMemory` and `endExecutableMemory`, and `structures <start> <size>` from `startOfStructureHeap` and `sizeOfStructureHeap` (N16); the engine image needs no line. `recordVMLayout` appends `heap <vm> <cell> <atom>`: `&vm`, the cell `jsEmptyString(vm)` and the `StringImpl` of `vm.propertyNames->length`, which `Identifier::fromString` allocates while the VM is constructed. They stand for the three kinds of heap target an image names: the VM's fields, its cells and the atoms. With `HeapProbes::Fixed` it writes `heap 1 2 3` in their place, as a heap that never moves would repeat them (H1). `placeholdersBeforeInitialize` reads the `pool` and `structures` lines of each file and, for each recorded range, maps `PROT_NONE` placeholders (`MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE`) over every gap of the range that `/proc/self/maps` lists as free, so neither reservation can land in it; the structure reservation is aligned to its own size (SPEC-image.md N22), so blocking the capturing process's range forces another base. `releasePlaceholdersAfterInitialize` unmaps them once both reservations exist.

The jsc shell calls the two placeholder functions around `JSC::initialize()` in `jscmain` and `recordLayout` right after, from the flags of section 5.1, and `recordVMLayout` in `runJSC` right after `VM::create`, with or without `--jitcache`. Bun's `JSCInitialize` does the same around `JSC::initialize(...)`, inside its `std::call_once`, from `--jitcache-twins-avoid-layout` and `--jitcache-twins-record-layout`, and `JITCacheHost::configureVM` calls `recordVMLayout` for the main thread's VM, whether or not JITCache is configured (SPEC-integrator.md section 11.2).

These functions run only in test processes, so a failure ends the process: a layout file that cannot be read or holds a malformed line, `/proc/self/maps` unreadable, a placeholder `mmap` that fails, or a layout line that cannot be written makes the function print the path and `strerror(errno)` to stderr and call `exit(1)`. The runner then fails the run on its exit code (section 7.5) instead of comparing a layout that did not move.

## 5. Shell, `bun:jsc` and `$vm` helpers

### 5.1 jsc flags

In twins builds `CommandLine::parseArguments` also recognizes:

| flag | effect |
|---|---|
| `--jitcache-twins-report=<path>` | `Config::twinReportPath` |
| `--jitcache-twins-record-layout=<path>` | `recordLayout(path)` after `JSC::initialize` |
| `--jitcache-twins-avoid-layout=<path>[,<path>...]` | placeholders over those processes' ranges around `JSC::initialize`; an empty value is an empty list, which run 0 of a sequence gets (section 7.3) |
| `--jitcache-test-fixed-heap-probes` | the shell calls `recordVMLayout` with `HeapProbes::Fixed` (section 4; H1) |
| `--jitcache-describe-heap=<path>` | writes the description of section 6 at the end of the run (SPEC-integrator.md section 11.1); it needs no `--jitcache` |
| `--jitcache-body-events=<path>` | writes the body-event dump of section 10.3 at the end of the run (SPEC-integrator.md section 11.1); it needs no `--jitcache` |
| `--jitcache-test-writer-fault=<check>@<n>` | the writer's fault injection (container sub-SPEC section 8.3) |
| `--jitcache-test-kill=<point>@<n>` | the writer's kill point (section 12; container sub-SPEC section 8.3), with `<point>` one of the names of section 12's table |
| `--jitcache-test-force-blinding` | `jscmain` calls `setForcesBlindingForTesting(true)` before `JSC::initialize`, which blinds every immediate the assembler considers for blinding; for runs with JITCache off only (section 11.5) |
| `--jitcache-test-store-fault=<call>:<error>[@<n>]` | the store's fault hook (container sub-SPEC section 7.5): `<call>` is `openBody` or `scoring`, since `bodyVersion` makes no system call, `<error>` is `EMFILE`, `ENFILE`, `ENOMEM` or `EIO`, and without `@<n>` every call fails |
| `--jitcache-test-image-hook=<name>` | `jscmain` calls `setImageTestHookNamed(name)` (`JITCacheTwinsHarness.h`) before `JSC::initialize`, which sets the Image lane's test hook for the process (SPEC-image.md R-INT-11 and section 11.3): `relocation-pairs`, `operation-pair`, `change-recorded-target` or `skip-patch`; any other name is malformed |
| `--jitcache-test-twin-entry=<kind>` | H1's own hook: right after `start` returns, when the shell VM's `twinReportSink()` is non-null, `runJSC` writes one entry of part `Integrator` and check `test-entry`, a difference for `difference`, a skip for `skip`, and a relocation coincidence for `coincidence:<domain>`, with a domain spelled as in section 2; any other kind is malformed |

### 5.2 jsc functions

`GlobalObject::finishCreation` (`jsc.cpp`) registers, in twins builds:

| function | length | behavior |
|---|---|---|
| `jitcacheICsSnapshot(fn, kind)` | 2 | `JSC::JITCache::ICs::functionSnapshotBaselineICs` (SPEC-ics.md section 11.1) |
| `jitcacheDelta()` | 0 | calls `delta`; `Completed` returns `{ eligibleKeys, committedBodies, committedBytes, deferredKeys }`; `Rejected` throws an `Error` whose message is `DeltaResult::rejection`, and `Faulted` one whose message is the fault's `stepName()` |
| `jitcacheStatus()` | 0 | the full step name of `status().firstFault`, or `null` |
| `jitcacheStartOutcome()` | 0 | `"started"`, `"busy"`, `"rejected"` or `"fault"` for the shell's `start`, or `null` when it made none |
| `jitcacheProgress()` | 0 | an object holding every field of `Progress` by name |
| `jitcacheBodyKey(fn, kind)` | 2 | the lowercase hex of the key recorded for the UCB of `fn`'s CB of `kind` (`"call"` or `"construct"`), using the CB's baseline alternative for an optimized CB, or `null` |
| `jitcacheBodyEvents(fn, kind)` | 2 | the event counts of the UCB of `fn`'s CB of `kind`, or `null` (section 10.2) |
| `jitcacheReadSection(keyHex, name)` | 2 | a new `ArrayBuffer` holding the section `name` (the lane's name of SPEC-integrator.md section 6.1) of the key's current body, or `null` when the body or the section is absent or the VM's cache activity is off |
| `jitcacheRewriteSection(keyHex, name, offset, bytes)` | 4 | the capture glue's `rewriteSectionForTesting` (SPEC-integrator.md section 8.3), which rewrites through the writer's `rewriteSection` (container sub-SPEC section 8.3) and, when it succeeds, forgets the key's kept summary, where `bytes` is an array of byte values; throws unless the shell's VM produces, and throws a failed rewrite's check and detail |
| `jitcacheDescribeHeap(...roots)` | 1 | the description of section 6 of the graph reachable from the given values, as a string; throws a `TypeError` without a value. The default roots serve only the shell's end-of-run description |

SPEC-ics.md R-INT-7 asks for the first three; the others serve the integrator's tests and the lanes' tests that rewrite a body (`jitcacheRewriteSection`, SPEC-ucb.md section 13.3) or compare heap state in the middle of a run. Plain builds register none of them (SPEC-integrator.md R-ALL-4).

### 5.3 Bun

In twins builds `bun:jsc` (`src/jsc/modules/BunJSCModule.h`) exports `jitcacheStatus`, `jitcacheProgress`, `jitcacheUCBStatistics`, `jitcacheDescribeHeap` and `jitcacheBodyEvents`. The module registers the host functions that `JITCacheTwinsHost.h` declares (SPEC-integrator.md section 3.5) and that the jsc shell registers under the same names, `JSC::JITCache::functionJITCacheStatus`, `functionJITCacheProgress`, `functionJITCacheUCBStatistics`, `functionJITCacheDescribeHeap` and `functionJITCacheBodyEvents`; the third returns what `$vm.jitCacheUCBStatistics()` returns. Bun's global object holds per-process state (`process.env`, `process.argv`, the pid), so a Bun script describes roots of its own, and the walk writes the global object it reaches through them without its edges (section 6.1). The module's export count grows by five in twins builds. Plain builds export none of them, so a test that calls one follows SPEC-integrator.md R-ALL-4.

### 5.4 `$vm`

`tools/JSDollarVM.cpp` gains, in twins builds, `$vm.jitCacheUCBSelfTest()` and `$vm.jitCacheUCBStatistics()` exactly as SPEC-ucb.md M4 specifies (SPEC-integrator.md section 14, M7). The runner passes `--useDollarVM=true`, a free option, to every twins-mode jsc run.

## 6. The reachable-heap description

Of the observables THREAD Verification compares, a script can print results, exceptions and stack traces but not the heap JavaScript can reach after a full collection: closure variables, internal slots and private names are invisible to reflection, and a graph walk written in JavaScript would run getters and proxy traps. The engine describes that heap instead, and the runner compares the descriptions (section 7.6). ([history](SPEC-integrator-history.md#the-oracle-compares-the-reachable-heap)) `JITCacheTwinsHarness.h` declares:

```cpp
// VM thread, API lock held. Runs a full collection, then describes; calls no getter, trap or host accessor (section 6.2).
String describeReachableHeap(JSGlobalObject&, std::span<const JSValue> roots);   // empty roots: the default roots of section 6.1
```

### 6.1 Roots and determinism

`describeReachableHeap` first runs `vm.heap.collectNow(Sync, CollectionScope::Full)`, then walks the graph under `DeferGCForAWhile`, so no collection interleaves with the walk. Given roots, it walks from them in order, and writes the global object, its `JSGlobalProxy` and its global lexical environment, unless they are among the roots, with their line and no edges: every function's scope chain ends there, so every root reaches them, and Bun's global object holds per-process state (section 5.3). The default roots are the global object, its global lexical environment (`JSGlobalObject::globalLexicalEnvironment()`) and the module environment (`AbstractModuleRecord::moduleEnvironment()`) of each module record the global object's loader holds (`JSModuleLoader::moduleMap()`, in `ModuleRegistryEntry::key()` order); from them the walk leaves out the global object's own property `arguments`, where the jsc shell puts the run's role and paths.

The description is a function of what JavaScript can observe, so two runs that perform the same JavaScript operations describe alike, whatever tier each body ran in and whatever their ICs, structures, storage and addresses. It holds no address, no structure identity or kind (shared, dictionary, poly-proto), no storage layout (inline or out-of-line slots, butterfly capacity, indexing type, copy-on-write), no string representation (rope, atom) and no cell JavaScript cannot reach (executables, CodeBlocks, UCBs, structures, symbol tables, property tables, JIT code). The walk reifies the lazy properties of what it reaches, the same ones in two such runs, through the host's own reification, which in Bun can run JavaScript: a `PropertyCallback` such as `Bun.sql`'s loads an internal module. The walk itself calls no getter, trap or host accessor (section 6.2), and a lazy property describes alike whether or not JavaScript read it before the walk. The shell describes its heap only once the script and `delta` are done (section 6.4). ([history](SPEC-integrator-history.md#the-oracle-compares-the-reachable-heap))

### 6.2 The walk

The walk is breadth-first and iterative. A primitive is written where it is met: `undefined`, `null` and booleans; numbers in shortest round-trip form, with `-0`, `NaN` and the infinities spelled out and no int32 or double distinction; BigInts in decimal; strings as JSON string literals with every lone surrogate escaped; and the empty value of an uninitialized binding as `<empty>`. Every other value is a cell, which takes the next ordinal the first time the walk meets it and is written `#<ordinal>` wherever it appears. The output is one line per cell, in ordinal order, each followed by one line per edge in the order the table gives. The exact text is the implementation's choice, since only descriptions from one build are compared.

| cell | the cell's line | its edges, in order |
|---|---|---|
| symbol | its description; its `Symbol.for` key when registered; whether it is private | none |
| any object | `JSCell::classInfo()->className`; callable; constructor; extensible as its structure says (`isStructureExtensible`), so no trap runs | the prototype (`getPrototypeDirect`); the own properties in the order `getOwnPropertyNames` gives them, private names included (`PropertyNameArrayBuilder` with `PropertyNameMode::StringsAndSymbols` and `PrivateSymbolMode::Include`); then the internal state the rows below add; then the private brands its structure carries: each private symbol the walk reached, its weak edges included, that `BrandedStructure::checkBrand` accepts, in their ordinal order |
| `JSFunction` with a `FunctionExecutable` | the provider's URL and the executable's start and end offsets, or a builtin's name | the scope (`JSCallee::scope()`) |
| `JSFunction` with a `NativeExecutable`, `InternalFunction` | its name | |
| `JSBoundFunction` | | target, bound this, bound arguments |
| `ProxyObject` | | target and handler (`null` once revoked), in place of the prototype and own properties, which only traps could read |
| `JSModuleNamespaceObject` | | in place of the own properties, each export by name, read from the binding it resolves to, so nothing is materialized and an empty binding writes `<empty>` |
| a `JSSymbolTableObject` scope: lexical, module or global lexical environment | the scope kind, in place of the any-object line | each variable its symbol table places in the scope, sorted by name, with its value; then the next scope (`JSScope::next()`); in place of the any-object edges |
| `JSWithScope` | the scope kind, in place of the any-object line | the object; the next scope; in place of the any-object edges |
| `JSMap`, `JSSet` | | the entries in iteration order |
| `JSWeakMap`, `JSWeakSet`, `JSWeakObjectRef`, `JSFinalizationRegistry` | | the weak edges of section 6.3 |
| `JSPromise` | its status | its result; not its reactions (below) |
| `JSInternalFieldObjectImpl` other than `JSPromise` and `ProxyObject` (iterators, generators) | | each internal field |
| `NumberObject`, `StringObject`, `BooleanObject`, `SymbolObject`, `BigIntObject` | | the primitive value |
| `DateInstance` | its time value | |
| `RegExpObject` | its pattern and flags | |
| `JSArrayBuffer` | shared, detached, byte length | the bytes, in hex |
| `JSArrayBufferView` | type, byte offset, length | the buffer, in place of the indexed own properties |
| any other cell, such as an engine cell an internal field holds | `JSCell::classInfo()->className` | none |

The rows below "any object" add to what an object writes, except where a row says it replaces a part. A scope replaces all of it: its own properties follow its symbol table's hash order, which an imported UCB's table need not share with the JITCache-off run's, and JavaScript cannot observe them. The global object is no scope these rows name, so it writes what any object does. `BrandedStructure` exposes its brands only through `checkBrand`, so brands are tested once the walk has ended and give no cell an ordinal. A class keeps its brand in a variable of its class scope (`BytecodeGenerator::emitCreatePrivateBrand`), so the walk reaches every brand a reachable function can check. A namespace that `import defer` created writes the single edge `exports <unevaluated>` in place of its exports until its module's cycle has evaluated without an error, the condition `ensureDeferredNamespaceEvaluation` tests, because reading their names would run that evaluation; the walk recognizes it by the `Symbol.toStringTag` its creation stores, `"Deferred Module"`, since `JSModuleNamespaceObject` keeps its deferral private. Each own property is read with a `PropertySlot` of type `VMInquiry`, which calls no getter, trap or host accessor. Its line gives the key, the attributes, and the value or the getter and setter; a custom value or accessor writes `<custom>`, and a property the inquiry cannot read writes `<opaque>`. An object of a class the table does not name contributes what any object does.

A pending promise's reactions are left out: JavaScript observes them only through what they run once the promise settles, which the rest of the output and the final description show.

### 6.3 Weak edges

A weak edge never gives a cell its ordinal. Whether a cell that only weak edges reach is still alive depends on the collector, its conservative stack scan included, and two runs JavaScript cannot tell apart can differ in it.

- A `WeakRef` writes its target's ordinal when the walk reached the target, and `<unreached>` otherwise.
- A weak map or weak set lists the entries whose key the walk reached, in the order of their keys' ordinals, each with its value. These lists come after the walk over strong edges ends; the cells first met in them are walked in turn, and the step repeats until it adds no cell.
- A finalization registry writes its cleanup callback and, sorted, the held value and unregister token of every registration, whether its target is alive or not: a primitive as written, and a cell by its ordinal when the walk reached it and `<cell>` otherwise. Its native lists are in hash order, so a held value only the registry reaches is listed but not walked.

### 6.4 Where it runs

The jsc shell writes the description of the default roots to the path `--jitcache-describe-heap` names, at the end of its run and after `delta` (SPEC-integrator.md section 11.1), so the description's collection changes no capture. `jitcacheDescribeHeap` runs it on demand in jsc and in `bun:jsc` (section 5). It needs no JITCache configuration, so a JITCache-off run writes it too.

## 7. The runner

### 7.1 Command line

`Tools/Scripts/run-jitcache-tests` is a TypeScript program run by `bun` (`#!/usr/bin/env bun`), like `build.ts`:

```text
run-jitcache-tests --build=<WebKit build directory> [--bun=<Bun executable>] [--mode=twins|plain]
                   [--pin=<the pin's WebKit build directory>] [--pin-options="<options>"]
                   [--extra-options="<options>"] [--filter=<regex>] [--jobs=<n>] [--timeout=<seconds>]
                   [--js-only | --cpp-only] [--qemu-cpu=<model>] [<test paths>...]
```

`--build` names `<build-root>/linux-<arch>-<profile>/deps/WebKit`, where it finds `bin/jsc` and `bin/testjitcache`. The mode defaults to `twins` when that build's `cmakeconfig.h` defines `ENABLE_JITCACHE_TWINS` as 1 and to `plain` otherwise; `--mode=plain` forces plain. `--bun` names the Bun executable for Bun-hosted scripts, which are skipped and listed without it. `--pin` names the build of section 11.1, against which the runner compares the off path of every jsc-hosted script (section 11); without it the comparison is skipped and listed. `--pin-options` adds options to this build's comparison runs alone, which only the self-tests of H7 use. `--extra-options` adds options to every run of every sequence, after the directory's defaults and before the run's own options, which is how a variant such as `--collectContinuously=true` or `--useUnlinkedCodeBlockJettisoning=true` reruns a whole directory (SPEC-ucb.md section 13.3). `--jobs` runs that many sequences at once, 5 by default; `--timeout` bounds each process, 600 seconds by default. When `--build`'s `<arch>` is `aarch64` on an x86_64 host, every process the runner starts from that build, from `--pin`'s or from `--bun` runs under QEMU user mode (section 7.9), and `--qemu-cpu` names its CPU model, `max` by default; elsewhere `--qemu-cpu` is malformed. The default test paths are `JSTests/jitcache/ucb`, `image`, `cb`, `ics` and `integrator`. The runner exits with 1 when any sequence or C++ group fails.

### 7.2 Directives

A script declares, in comment lines within its first 50 lines, the directives below. A `<run index>` counts from 0 within a sequence. Alone it names that run in every sequence of the script; written `<sequence>:<run>`, it names that run of one sequence, with sequences counted from 0 in the order of their `jitcache-runs` lines. ([history](SPEC-integrator-history.md#directives-address-runs-by-sequence))

- `// jitcache-runs: <run>; <run>; ...`, one line per sequence. A run is a role, `Off`, `Producer`, `ConsumerProducer`, `Consumer` or `Maintenance`, followed by options. For `Maintenance` the rest of the run is a maintenance command line in which `$ARTIFACT` stands for the sequence's artifact path.
- `// jitcache-host: jsc` (the default) or `// jitcache-host: bun`.
- `// jitcache-requires: twins`: the script runs only in twins mode.
- `// jitcache-expect-exit: <run index> <code>`: the run at that index, counted from 0, must exit with that code, or end by `SIGABRT` when the code is `abort` and by `SIGKILL` when it is `kill` (section 12).
- `// jitcache-expect-fault: <run index> <step>`: the run at that index may report the fault whose full step name (`FaultReport::stepName()`) is `<step>`, such as `budget.limit` or `exec-alloc.dfg-plan`. A run that may report one of several faults carries one directive per step, and any fault that none of them names fails the run (section 7.5).
- `// jitcache-require-fault: <run index> <step>`: allows the step as `jitcache-expect-fault` does, and requires at least one named run to report it. The runner checks the requirement after the script's last sequence, so a bare index spans every sequence, and a sequence prefix narrows it to one.
- `// jitcache-expect-no-install: <run index>`: the `Consumer` or `ConsumerProducer` run at that index need not install a body (section 7.5), as when the producer before it faulted before committing one or the run's own `start` is rejected on purpose.
- `// jitcache-expect-twin: <run index> <kind> <domain>`: the twin report of the run at that index must hold a report of that kind, spelled as in section 2. A `coincidence` names its domain; a `difference` or a `skip` names its part in the domain's place. The directive declares every report of its kind and part, or of its domain, in that run: at least one is required, and none fails the run (section 7.5). A test hook that forces reports, such as SPEC-image.md T3's, declares them this way.
- `// jitcache-check: <run index> <checker>`: after that run, the runner executes `bun <checker> <stdout file> <stderr file> <scratch>`, where `<checker>` is a path relative to the script, and fails the run when the checker exits nonzero. A test that must read what the engine prints to stderr, such as the `verboseOSR` lines of SPEC-cb.md section 11.3, checks it this way.
- `// jitcache-heap: off <reason>`: the oracle compares this script's output but not its heap (section 7.6). A script uses it only when it cannot keep a value that depends on its role out of the heap it leaves, and says why.
- `// jitcache-pin: off <reason>`: the pin comparison (section 11) skips this script. A script uses it only when its `Off` run cannot run on the pin, because it calls a function only this build has; when the two native fixes change its optimizing compiles (section 11.4); or when its runs decode the jsc shell's bytecode cache, whose payload `computeJSCBytecodeCacheVersion` ties to the build that wrote it, so the pin's build generates where this build decodes. It says which.
- `// jitcache-qemu-cpu: <run index> <model>`: under QEMU the run at that index uses that CPU model in place of `--qemu-cpu`'s (section 7.9). A script that declares it runs only under QEMU; elsewhere the runner skips and lists it.

### 7.3 Sequences and runs

Each sequence gets a fresh temporary directory holding `artifact/`, the parent path JITCache receives, and `scratch/`, which every run of the sequence shares. A jsc run's command line is:

```text
<jsc> --destroy-vm
      [--jitcache=<artifact> --jitcache-mode=<p, c or p-c for the run's role> --jitcache-strict=1 --jitcache-log]
      [twins mode: --useDollarVM=true --jitcache-describe-heap=<scratch>/run<i>.heap
                   --jitcache-twins-report=<scratch>/run<i>.twins
                   --jitcache-twins-record-layout=<scratch>/run<i>.layout
                   --jitcache-twins-avoid-layout=<the layout files runs 0 to i-1 wrote>]
      [the directory's defaults (section 7.4)]
      <the run's options> <script> -- <role> <scratch> <artifact> <sequence>
```

`--jitcache-log` makes the shell write its final status to standard error once the script and `delta` are done (SPEC-integrator.md section 11.1), which section 7.5 reads. An `Off` run omits the JITCache flags of the first bracket and the twin report and layout flags; in twins mode it keeps `--useDollarVM=true` and `--jitcache-describe-heap`, which configure no JITCache. The run's options come last so that they override a default, such as `--jitcache-strict=0` or `--jitcache-max-memory=4096`. `Off` and `Maintenance` runs write no layout, so the list holds only the files earlier runs wrote: a missing file would end the run at `exit(1)` (section 4). Avoiding the layouts of every earlier run that wrote one moves the executable pool and the structure reservation between each importing process and each earlier process whose captures it imports, a ConsumerProducer's recaptures included (SPEC-image.md R-INT-11 and T15); randomization moves the heap (section 4). A `Maintenance` run is `<jsc> --jitcache-maintenance -- <command>`. A script reads its role, its scratch path, its artifact path and its sequence index as `arguments[0]` to `arguments[3]`. Every run of a sequence receives that sequence's index, the oracle runs of section 7.6 included, so a script can pick the case a sequence tests. It passes them to a function that holds its body, as in `(function main(role, scratch, artifact, sequence) { ... })(...arguments)`, so that no binding or property that outlives the run depends on the role (section 7.6).

Before its first twins-mode sequence, the runner calibrates the heap (section 4). In a fresh temporary directory it starts `<jsc> --useDollarVM=true --jitcache-twins-record-layout=<dir>/calibration<i>.layout <the extra options> -e ""` for i = 0 and then 1, and compares the two `heap` lines. When no probe repeats, the build's allocator moves the heap, and the runner goes on. When one repeats, that allocator maps its space at a fixed base on this machine, and the runner calibrates again with `WebKitMallocForceEnabled=1` in both processes' environment, which hands the JSC heap to libpas (N17). If the probes then differ, every twins-mode jsc and Bun process the runner starts gets that variable; if one still repeats, the runner stops before any sequence with an error naming the heap domain. ([history](SPEC-integrator-history.md#twins-builds-keep-buns-build-flags-and-the-runner-moves-the-heap))

### 7.4 Directory defaults and skipped checks

| directory | sequence when the script declares none | what the runner adds in twins mode | twins required |
|---|---|---|---|
| `ics/` | `Producer; Consumer` | `--useConcurrentJIT=false`, unless the run's options set `useConcurrentJIT` | yes |
| `ucb/`, `image/`, `cb/` | `Producer --jitcache-delta-at-exit; Consumer` | `--useConcurrentJIT=false`, unless the run's options set `useConcurrentJIT` | no |
| `integrator/` | `Producer --jitcache-delta-at-exit; Consumer` | `--useConcurrentJIT=false`, unless the run's options set `useConcurrentJIT` | yes: its scripts read their outcomes through the functions of section 5.2, which only twins builds register |

A twin check that skips itself fails the run exactly when concurrent JIT is off in the process that imports the body and in every process whose captures it imports (THREAD Verification). The image check, the one check that skips, does so when the importing process runs with concurrent JIT or the capture's compilation ran on a JIT worker thread (SPEC-image.md section 11.3), and SPEC-image.md R-INT-11's item for the runner turns concurrent JIT off in every process of a twins run. A skip in a run's twin report therefore fails the run exactly when that run and every earlier run of its sequence that produced, as a `Producer` or a `ConsumerProducer`, have an effective `useConcurrentJIT` of false, since those are the importing process and the processes whose captures it can import; otherwise the runner lists the skip and goes on. Every directory's twins runs turn it off unless a run's options set it, so the image check compares every import the corpus makes and a skip in those runs fails them; a run that turns concurrency on in its options, as the CB lane's concurrency cases and SPEC-image.md T14 do, treats a skip as no difference. ([history](SPEC-integrator-history.md#when-a-skip-fails-a-run))

### 7.5 Checks

After each run: the exit code is 0, or the one `jitcache-expect-exit` names, and every `jitcache-check` checker of the run passes; the runner keeps each run's standard output and error in `scratch/run<i>.stdout` and `.stderr`. A run that fails the exit code only because LeakSanitizer reported leaks at exit passes it when those leaks are the engine's own, which JITCache does not fix (skills/SKILL.md, Scope): in a run that configures JITCache, when its oracle run (section 7.6) reports each of them too, the same bytes allocated from the same stack; in an `Off` run, always, since it is the native reference itself. A leak a run's oracle does not report fails the run. In a twins build the UCB lane's twin check parses each imported body again (SPEC-ucb.md section 13.2), so an imported class field initializer leaks its field names as its native parse does. When the body enclosing the class was imported as well, the decode allocated those names first, and the leak shows the decode's stack, which no oracle reports; a script whose field initializers import inside an imported body names those fields with identifiers the VM allocates when it is created, such as those of `CommonIdentifiers`. ([history](SPEC-integrator-history.md#leaks-the-oracle-run-also-reports-are-the-engines))

In both modes, for a run with JITCache, the runner reads every final status the run wrote to standard error, a `{"jitcache":"status", ...}` line of `toJSON` (SPEC-integrator.md sections 3.1 and 11.1); the jsc shell writes one for its main VM, and Bun, in twins mode, one for each VM at exit (section 7.7). A run that `jitcache-expect-exit` expects to end by a signal writes none, and these checks skip it, as they skip a Bun-hosted run in plain mode. Any other run fails when it wrote none; when a status reports a fault, as its `activityFault` or its `productionFault`, whose `step` no `jitcache-expect-fault` or `jitcache-require-fault` directive of the run names; and when it is a `Consumer` or `ConsumerProducer` run that `jitcache-expect-no-install` does not name and the `installs` of its statuses' `progress` add up to zero. A strict check that fails at an import or an install turns cache activity off and leaves the run on the native path, whose output still matches the oracle and whose later bodies meet no twin check, so these checks fail a run that the oracle and the twin report would pass. After a script's last sequence, the runner fails the script when some `jitcache-require-fault` step was reported by none of the runs it names. ([history](SPEC-integrator-history.md#the-runner-fails-undeclared-faults-and-runs-that-install-nothing))

In twins mode, for a run with JITCache: its twin report exists, and holds at least one report of each `jitcache-expect-twin` directive of the run, or the run fails. A declared report neither fails nor marks anything. Every other report follows these rules: any `difference` fails the run; a `skip` follows section 7.4; a `coincidence` in the `heap` domain marks the sequence; and any other `coincidence` fails the run. Placement moved the `executable-pool` and `structure-reservation` domains between processes, the check skips every target of an engine at its link-time address, so it reports the `engine-image` domain only for an engine loaded elsewhere, and an import of a body the run's own process captured reports no pair (SPEC-image.md section 11.4). ([history](SPEC-integrator-history.md#imports-a-process-captured-itself))

A sequence whose only failures are marked heap coincidences is run once more, with fresh directories and processes, and any coincidence in that repetition fails it (SPEC-image.md R-INT-11).

### 7.6 The oracle

This is THREAD Verification's oracle. After each sequence, the runner compares every run that configures JITCache, each `Producer`, `ConsumerProducer` and `Consumer` run, with an `Off` run of section 7.3 whose run options (the `<the run's options>` of section 7.3, not the brackets the runner adds) are that run's minus every option that begins with `--jitcache`. It starts one such oracle run for each distinct option set among them, numbered j from 0, and in twins mode gives each `--jitcache-describe-heap=<scratch>/oracle<j>.heap`, a path no run of the sequence writes. For each pair it compares: ([history](SPEC-integrator-history.md#the-oracle-compares-every-run-that-configures-jitcache))

- their standard output, byte for byte, which carries the results, exceptions and stack traces the script prints;
- in twins mode, for a jsc-hosted script that does not declare `jitcache-heap: off`, their heap descriptions, the run's `run<i>.heap` against its oracle run's `oracle<j>.heap` (section 6), byte for byte.

A run that `jitcache-expect-exit` expects to end by `abort` or `kill` writes no heap description, since the shell writes it after the run loop, so the oracle compares its output only, as section 7.5 skips its status checks. Its oracle run carries no kill flag and runs to the end, so such a script prints nothing after the point where the signal can strike. A difference fails the sequence (THREAD Verification). The comparisons of SPEC-ics.md T6 are these pairs. A script therefore follows SPEC-integrator.md R-ALL-4 and prints what it compares, stack traces included; holding its body in a function of the role and paths (section 7.3) keeps the role out of the heap, since the description leaves out the shell's `arguments` (section 6.1). In plain mode the oracle compares the output only.

### 7.7 Bun-hosted runs

A Bun-hosted run executes `<bun> <JITCache flags> <script> <role> <scratch> <artifact> <sequence>`. The runner passes the JITCache flags of section 7.3 and every run option that begins with `--jitcache` through as they are, since Bun takes the shell's flags (SPEC-integrator.md section 11.2), and drops three: `--jitcache-delta-at-exit`, since Bun calls `delta` at exit for a producing role, `--jitcache-log`, which Bun does not take, and `--jitcache-describe-heap`, since a Bun run's oracle compares standard output only (below). Bun takes no `--jitcache-max-memory` either, so the runner fails a script that gives one to a Bun-hosted run as malformed. Of section 5.1's flags, Bun takes `--jitcache-twins-report`, `--jitcache-twins-record-layout`, `--jitcache-twins-avoid-layout` and `--jitcache-body-events` alone, and `--jitcache-maintenance` has no Bun equivalent, since Bun's maintenance is `bun jitcache` (SPEC-integrator.md section 11.2): the runner fails a script that passes any other flag of section 5.1, or `--jitcache-maintenance`, to a Bun-hosted run as malformed. The environment carries `BUN_DESTRUCT_VM_ON_EXIT=1` in place of `--destroy-vm`, `WebKitMallocForceEnabled=1` when the calibration of section 7.3 chose it, and `BUN_JSC_useConcurrentJIT=0` under the same rule as section 7.4. Every other run option `--<option>=<value>` is passed as `BUN_JSC_<option>=<value>`: Bun's `JSCInitialize` hands a `BUN_JSC_` variable that `Options::setOption` rejects to `onCrash`, so no JITCache flag may take that route. The main VM writes its twin report at the configured `<scratch>/run<i>.twins`, each worker VM at `<scratch>/run<i>.twins.<executionContextId>`. The runner applies section 7.5's twin-report checks to every one of those files the run wrote, so a worker's imports meet their twins as the main VM's do (THREAD Verification); a run with JITCache still needs the main VM's report. In twins mode each VM writes its final status at exit, since the run has `--jitcache-twins-report` (SPEC-integrator.md section 11.2), and section 7.5's status checks read every one, a worker's included; in plain mode Bun writes none, so those checks apply to jsc-hosted runs only. The script reads its role, scratch and artifact paths and its sequence index from `process.argv[2]` to `[5]`. The oracle run is `bun <script>` without the JITCache flags, and compares standard output; a Bun script that compares heap state prints `jitcacheDescribeHeap` of its own roots (section 5.3).

### 7.8 C++ tests

The runner calls `testjitcache --list`, which prints one `name<TAB>options` line per test, then runs `testjitcache --group=<options>` once per distinct options string, up to `--jobs` at once; a group fails on a nonzero exit. A C++ test, and the UCB self-test, has no oracle run to tell the engine's leaks from JITCache's, so it avoids input on which the engine leaks with JITCache off, such as a class field initializer's body.

### 7.9 Under QEMU

HARNESS.md runs ARM64 on this machine's x86_64 host under QEMU user mode. ([history](SPEC-integrator-history.md#under-qemu-the-runner-moves-the-heap-with-the-guest-stack))

- N26. QEMU user mode (`qemu-aarch64` 8.2.2 here) places every guest mapping itself, upward from a fixed base with the guest stack first, and never randomizes them, so two runs of one script repeat every guest address, with `WebKitMallocForceEnabled=1` or without. A guest stack n bytes larger (`-s`) moves every later mapping n bytes up, except where an allocator's alignment absorbs the step: ASan's primary allocator aligns its space to 128 KiB, and a 1 MiB step left the cells of Bun's ASan build in place where a 16 MiB step moved them. `-B` and the environment's size move nothing the guest sees. QEMU honours `MAP_FIXED_NOREPLACE` and lists the guest's mappings in its emulated `/proc/self/maps`, so section 4's placement works unchanged. A guest cannot start another aarch64 process, since QEMU follows no `exec` and this host registers no binfmt handler for aarch64.
- N27. `qemu-aarch64 -cpu` decides the HWCAP bits `MacroAssemblerARM64::collectCPUFeatures` reads: `cortex-a53` sets none of LSE, JSCVT, FP16, FRINTTS, SHA3 and DotProd, `neoverse-n1` sets LSE, FP16 and DotProd, `neoverse-v1` adds JSCVT and SHA3, and `max`, the model without `-cpu`, sets all six. The mimalloc bundled in bmalloc turns `MI_OPT_ARCH` on for arm64 and compiles with `-march=armv8.1-a` (`Source/bmalloc/mimalloc/mimalloc/CMakeLists.txt`), so the release build, which uses it, executes an LSE instruction in `mi_process_init` and dies of `SIGILL` under `cortex-a53`; the ASan builds of jsc and Bun use no mimalloc and run under every model.

The runner starts every process of an aarch64 build, the calibration's of section 7.3 and the oracle's included, as `qemu-aarch64 -L <root> -cpu <model> -s <n>M <executable> <arguments>`:

- `<root>` is `JITCACHE_AARCH64_ASAN_ROOT`, by default `~/jitcachearm/arm64-glibc-root`, which `~/jitcachearm/arm64-glibc-root.sh` provisions, when the build's `CMakeCache.txt` sets `ENABLE_SANITIZERS` to `address`, because its ASan runtime needs glibc 2.34 or newer; otherwise `JITCACHE_AARCH64_SYSROOT`, by default `~/collo-local/tools/linux-sysroot-glibc-arm64`, the sysroot `build.ts` links against.
- `<model>` is the run's `jitcache-qemu-cpu` model, or `--qemu-cpu`'s.
- `<n>` is 8 + 64·k, with k drawn at random from 1 to 64 for each process, different from every k already drawn in its sequence or its calibration, and drawn again when section 7.5 repeats the sequence. QEMU would place the heap at the same addresses in every process (N26), and the stack, which it maps before ASan reserves its space, is the one input that moves them; 64 MiB exceeds every alignment measured, and the calibration and H2 check that the step moves the build's heap. The size is written in `M`, because `qemu-aarch64` 8.2.2 reads `-s 1G` as a 128 KiB stack. JSC uses at most `maxPerThreadStackUsage` (5 MiB) of a stack, so no result depends on the size.
- For an ASan build, `ASAN_OPTIONS` gets `detect_leaks=0` appended after any value it holds, because LeakSanitizer cannot start its tracer under QEMU. The rest of the environment, `WebKitMallocForceEnabled` and Bun's variables included, reaches the guest unchanged.

A Bun-hosted script that starts another engine process cannot run under QEMU (N26).

## 8. The C++ test framework

### 8.1 `tests/JITCacheTest.h`

```cpp
#if ENABLE(JITCACHE_TWINS)
namespace JSC::JITCache::Tests {

enum class NeedsVM : bool { No, Yes };

class TestContext {
    WTF_MAKE_NONCOPYABLE(TestContext);
public:
    explicit TestContext(VM*);     // testjitcache's main: the test's fresh VM, or null
    VM* vm() const;                // NeedsVM::Yes: a fresh VM whose API lock the test holds; null otherwise
    void fail(const char* file, int line, String message);
    bool failed() const;
    const Vector<String>& messages() const;   // what fail recorded, which main prints after FAIL <name>

private:
    VM* const m_vm;
    Vector<String> m_messages;
};

using TestFunction = void (*)(TestContext&);

struct TestRegistration {
    TestRegistration(ASCIILiteral name, NeedsVM, ASCIILiteral options, TestFunction);
};

}

#define JITCACHE_TEST_WITH_OPTIONS(name, needsVM, options) \
    static void name(JSC::JITCache::Tests::TestContext&); \
    static JSC::JITCache::Tests::TestRegistration name##Registration { #name ""_s, JSC::JITCache::Tests::NeedsVM::needsVM, options ""_s, name }; \
    static void name(JSC::JITCache::Tests::TestContext& context)
#define JITCACHE_TEST(name, needsVM) JITCACHE_TEST_WITH_OPTIONS(name, needsVM, "")
#define JITCACHE_CHECK(condition) \
    do { if (!(condition)) context.fail(__FILE__, __LINE__, "check failed: " #condition ""_s); } while (false)
#define JITCACHE_FAIL(message) context.fail(__FILE__, __LINE__, message)
#endif
```

A test is a function registered by name, with a flag saying whether it needs a VM and the JSC options its process needs, and fails through a macro that records a message (SPEC-ics.md R-INT-9). The options string is a space-separated list of `--name=value` JSC options; tests with the same string share a process.

### 8.2 `tests/testjitcache.cpp`

`main` accepts `--list`, `--group=<options>` (the empty group when absent) and `--filter=<substring>`. It calls `JSC::Config::enableRestrictedOptions()` and `WTF::initializeMainThread()`, as `jscmain` does, and then `JSC::initialize` with a customization callback that applies the group's options with `Options::setOptions`, as Bun's `JSCInitialize` and the `testb3` and `testmasm` drivers apply theirs. `Options::initializeWithOptionsCustomization` runs that callback once every option holds its default and `OptionsHelper::initialize` has constructed the override bookkeeping that `Options::setOptions` writes (`OptionsHelper::setWasOverridden`), and before `notifyOptionsChanged` derives the dependent options. When `Options::setOptions` rejects the group string, the callback records it, and `main` prints it and exits with 2 once `JSC::initialize` returns. ([history](SPEC-integrator-history.md#testjitcache-applies-options-in-the-initialization-callback))

For each registered test of the group whose name contains the filter, `main` creates a fresh VM with `VM::create(HeapType::Large)` when the test needs one, takes `JSLockHolder` and runs the test. It drops its own reference to the VM before the holder goes, so the holder's reference is the last, which `JSLockHolder::~JSLockHolder` drops before unlocking: `VM::~VM`, which asserts that the thread holds the API lock, runs under the lock, as in `runJSC` under `--destroy-vm`. Each test that needs a VM is thus "created after `JSC::initialize()` with default options and without a `start` call, with its API lock held" (SPEC-ics.md R-INT-9), apart from the options its registration names, and is the only VM in the process (SPEC-cb.md U4). `main` prints `PASS <name>` or `FAIL <name>` followed by each recorded message, and exits with 1 when any test failed.

### 8.3 The target

`Source/JavaScriptCore/shell/CMakeLists.txt` declares the target in two places, as it does `testFFI` (SPEC-integrator.md section 14, M9). Right after the `testFFI` declaration block:

```cmake
if (ENABLE_JITCACHE_TWINS)
    set(testjitcache_SOURCES
        ../jitcache/tests/testjitcache.cpp
        ../jitcache/tests/IntegratorTests.cpp
        ../jitcache/tests/ContainerTests.cpp
        ../jitcache/tests/StoreTests.cpp
        ../jitcache/tests/WriterTests.cpp
        ../jitcache/tests/MaintenanceTests.cpp
        ../jitcache/tests/ImageSectionTests.cpp
        ../jitcache/tests/ImageRecordingTests.cpp
        ../jitcache/tests/JITCacheCBAccessorTests.cpp
        ../jitcache/tests/JITCacheCBFormatTests.cpp
        ../jitcache/tests/JITCacheCBCaptureTests.cpp
        ../jitcache/tests/JITCacheCBSummaryTests.cpp
        ../jitcache/tests/JITCacheCBImportTests.cpp
        ../jitcache/tests/JITCacheCBTwinsTests.cpp
        ../jitcache/tests/ICSectionTests.cpp
        ../jitcache/tests/ICLiveTests.cpp
    )
    set(testjitcache_DEFINITIONS ${jsc_PRIVATE_DEFINITIONS})
    set(testjitcache_PRIVATE_INCLUDE_DIRECTORIES ${jsc_PRIVATE_INCLUDE_DIRECTORIES} "${JAVASCRIPTCORE_DIR}/jitcache/tests")
    set(testjitcache_FRAMEWORKS ${jsc_FRAMEWORKS})
    set(testjitcache_LIBRARIES ${CMAKE_DL_LIBS})
    if (USE_EXTERNAL_MIMALLOC)
        list(APPEND testjitcache_LIBRARIES $<TARGET_OBJECTS:mimalloc-obj>)
    endif ()
    WEBKIT_EXECUTABLE_DECLARE(testjitcache)
endif ()
```

and right after `WEBKIT_EXECUTABLE(testFFI)` and its `endif ()`:

```cmake
if (ENABLE_JITCACHE_TWINS)
    WEBKIT_EXECUTABLE(testjitcache)
endif ()
```

`WEBKIT_EXECUTABLE_DECLARE` creates the target around `cmakeconfig.h` alone, and `WEBKIT_EXECUTABLE` attaches the sources, definitions, include directories, frameworks and libraries set above (`Source/cmake/WebKitMacros.cmake`). The sources are every C++ test file the parts' SPECs name (SPEC-image.md section 14.2, SPEC-cb.md section 9.4, SPEC-ics.md section 13 and this SPEC), stubbed until their owners write them (SPEC-integrator.md section 14, M9). This is the unit-test target SPEC-cb.md R-INT-10, SPEC-ics.md R-INT-9 and SPEC-image.md R-INT-11 require.

## 9. Bench reports

### 9.1 Format

When `Config::benchReportPath` is set, the `VMState` keeps a `BenchReport` (`JITCacheBench.h`) that appends JSON lines to that path, buffered and flushed when the buffer passes 1 MiB, after each `delta`, at each host's exit (`flushBenchReport`, SPEC-integrator.md section 11), and in `willDestroyVM`. Every line holds `event`, `pid` and the VM's ordinal in the process, then the event's fields. Times are nanoseconds of `CLOCK_THREAD_CPUTIME_ID` on the thread that did the work, sizes are bytes, and keys are lowercase hex. On Linux the vDSO does not serve that clock, so each read is a system call that lands inside the span it measures. A time that counts toward THREAD's installation bound therefore reads that clock only where a counted span begins or ends, which for the install function is one pair around its whole span (section 9.2), and a breakdown of a counted span into steps reads `CLOCK_MONOTONIC` instead (`MonotonicTime::now()`, which the vDSO serves), as wall time that only explains the total. The report is off by default, and the bench loop turns it on; it is not charged to the producer budget (SPEC-integrator.md section 4.4). ([history](SPEC-integrator-history.md#measurement-choices))

### 9.2 Events

| event | written | fields |
|---|---|---|
| `start` | at the end of `start` | outcome, role, CPU time, indexed bodies |
| `lookup` | at each flush | counts and CPU time of `bodyVersion` for absent and for present keys, and of `openBody` by result |
| `open` | each `openBody` that maps a file | key, bytes, CPU time |
| `request` | each request point that imports, seeds or attaches a UCB, recorded by the UCB lane (SPEC-ucb.md B2) | key; what the request point did (`import`, `seed` or `attach`); the time of each part B2 lists, read from `CLOCK_MONOTONIC` (section 9.1); the total of the parts that count toward THREAD's installation bound and, apart, the total of those THREAD measures separately |
| `install` | each `Installed` outcome | key; the total, the install function's thread CPU time from its start to the close of step 18, one pair of reads (section 9.1); the time of each step of SPEC-integrator.md section 7.2, each lane's call apart (image parse, image validation, baked facts, gate, `prepareImage`, `CBStateImport::prepare`, `prepareBaselineICs`, `seedLinkedState`, `seedCallLinkHistory`, `commit`, setup, `attachPropertyICState`, `finishCounter`, `installCode`), read with `CLOCK_MONOTONIC`; `bodyRelease`, the thread CPU time of dropping the function's reference to the body after the total closes, which unmaps the body when that reference is the last (SPEC-integrator.md IB1); the `CBCounterRestore` (`carried`, which says whether the import carried counter progress, `crossed`, `nativeSlice`, `slice`); `codeSize`, the size of the installed `BaselineJITCode`'s executable memory (`JITCode::size()`), and beside it the image section's code size and fixup count; whether per-VM support was generated during the install function (`supportGenerated`, section 9.3) |
| `compile` | each finalize capture hook | the key the parent-key registry holds for the CB's UCB (`UCBRegistry::keyOf`), or `null` when it holds none, whether or not the code carries an image record; the compilation's CPU time after its profile drain; the finalization's CPU time without relinking incoming calls; `codeSize`, measured as in the `install` event, so the two events of one body report one size; whether per-VM support was generated during the compilation or the finalization (`supportGenerated`) |
| `capture` | each scored capture, at the finalize hook or in `delta` | trigger, key, and whether it was committed, beaten or deferred; the candidate's score fields, with `UCBRichness::exitSiteUnits` apart, and, when a saved body was scored, its score fields and the first field in `beats` order in which the two differ (SPEC-integrator.md IB10); the `hasPolymorphicSite` the glue passed to the CB lane; the CPU time of each lane's scoring call (`liveRichness`, `summarizeBaselineICs`, `scoreLive`) and of the scoring read with its `scoreSections`; the CPU time of each lane's build (`captureImage`, `buildSections` with `liveRichness`, `captureBaselineICs`, `CBStateCapture::capture`); the writer's fields, from the `CommitTiming` the glue passes to `commit` when the report is open (SPEC-integrator.md section 6.3): each section's stream time beside its size, and the reread and publish times; for a committed capture, `charges.budget`, the budget's charge right after the commit while every charge the capture made is still held, `charges.keptSummaries` and `charges.indexEntry`, which with the sections' sizes, `writerStagingBytes` and the `budget` line give SPEC-integrator.md IB6's parts |
| `delta` | each `delta` | CPU time of each phase, eligible keys, committed bodies and bytes, deferred keys; `codeBlocks`, the CBs phase 1 walked; `candidates`; `candidateTableBytes`, the candidate table's charge when phase 1 ends, its largest, since the table only grows |
| `index` | at each flush, when the state handed the report an artifact (`setArtifact`) | the totals of the process for that artifact, which every VM of the process that holds it repeats (container sub-SPEC section 5.2, `IndexStatistics`): `inotify`; the index's `bodies` and `tableBytes`; `buildNanoseconds`, the listing that built it; `listings.count`, `.nanoseconds` and `.failed`; `queueOverflows`; `refreshes.count`, `.nanoseconds` and `.deferred`; `deferredMisses`; `writerUpdates.count`, `.timed` and `.nanoseconds`. Its times are `CLOCK_MONOTONIC` wall time (section 9.1), since this work runs inside spans that the lookups and the commit measure with the thread CPU clock |
| `budget` | at each flush | limit, charged, peak |
| `process` | at each flush | the process's peak resident set (`getrusage(RUSAGE_SELF)`, `ru_maxrss`) and the executable pool's committed bytes (`ExecutableAllocator::committedByteCount`), which HARNESS.md reports beside every bench number |

`JITCacheBench.h` declares:

```cpp
namespace JSC::JITCache {

struct BenchField {
    ASCIILiteral name;   // a nested field joins its parts with '.', as "restore.carried"
    Variant<std::nullptr_t, bool, int64_t, uint64_t, double, String> value;   // WTF's Variant, which WTF::switchOn visits; a key as a lowercase hex String
};

class BenchReport final {
    WTF_MAKE_NONCOPYABLE(BenchReport);
    WTF_MAKE_TZONE_ALLOCATED(BenchReport);
public:
    // start's step 3 (SPEC-integrator.md section 3.2): appends to path; null when the file cannot be opened. The report
    // takes the process's next VM ordinal, which every line carries.
    static std::unique_ptr<BenchReport> open(const String& path);
    ~BenchReport();   // writes the lines still buffered, without the summary lines, and closes the file

    void record(ASCIILiteral event, std::initializer_list<BenchField>);   // VM thread; flushes past 1 MiB
    void flush();                                   // VM thread: the buffered lines, then the lookup, index, budget and process lines
    void addRelinkNanoseconds(uint64_t);            // VM thread: the relink accumulator of section 9.3
    uint64_t takeRelinkNanoseconds();               // VM thread: returns the accumulator and resets it

    // VM thread. The budget whose limit, charged and peak bytes each budget line writes; the state hands it its producer
    // budget, and a report without one writes zeros.
    void setBudget(RefPtr<ProducerBudget>&&);

    // VM thread. The VM's own artifact, whose IndexStatistics each index line writes; a report without one writes no
    // index line.
    void setArtifact(RefPtr<OpenedArtifact>&&);

    // VM thread. VMState::bodyVersion and VMState::openBody count each call and its CPU time here (SPEC-integrator.md
    // section 6.2), and each flush writes the totals in the lookup line.
    enum class Lookup : uint8_t { BodyVersionAbsent, BodyVersionPresent, OpenBodyFound, OpenBodyMissing, OpenBodyUnusable };
    static constexpr unsigned numberOfLookups = 5;
    void countLookup(Lookup, uint64_t nanoseconds);

private:
    BenchReport(int fd, unsigned vmOrdinal);

    struct LookupTally { uint64_t count { 0 }; uint64_t nanoseconds { 0 }; };

    const int m_fd;                                 // opened with O_APPEND
    const int m_pid;
    const unsigned m_vmOrdinal;
    StringBuilder m_buffer;                         // the lines not yet written
    uint64_t m_relinkNanoseconds { 0 };
    RefPtr<ProducerBudget> m_budget;
    RefPtr<OpenedArtifact> m_artifact;
    std::array<LookupTally, numberOfLookups> m_lookups { };
};

BenchReport* benchReport(VM&);   // null when no report is open

// Any thread: CLOCK_THREAD_CPUTIME_ID in nanoseconds, the CPU time of section 9.1.
uint64_t benchThreadCPUNanoseconds();

// A key as every event writes it: the lowercase hex of its 40 canonical bytes, its body file's name without ".bin".
String bodyKeyHex(const BodyKey&);

class RelinkTimer {
    WTF_MAKE_NONCOPYABLE(RelinkTimer);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit RelinkTimer(VM&);   // reads the thread CPU clock when benchReport(vm) is non-null and
                                 // !vm.heap.currentThreadIsDoingGCWork(); otherwise measures nothing
    ~RelinkTimer();              // when it measured, adds the elapsed time to that report's relink accumulator

private:
    BenchReport* m_report { nullptr };   // null when it measures nothing
    uint64_t m_startNanoseconds { 0 };
};

}
```

Any part may call `record` for the measurements its own bench obligations name, at the cost of one load and one test when the report is off. The `install` and `capture` events carry the integrator's timings of the lanes' calls that SPEC-cb.md R-INT-11, SPEC-ics.md B1 and B2 and SPEC-image.md B1 and B4 ask the glue for. The `install` event's per-call times are wall time; its total stays thread CPU time, the measure THREAD's installation bound uses.

### 9.3 Hooks for the native cost

These hooks measure the native cost THREAD's opening defines. ([history](SPEC-integrator-history.md#measurement-choices))

- `BaselineJITPlan` gains `bool m_jitCacheMeasure`, set in its constructor on the VM thread when `benchReport(vm)` is non-null, and `uint64_t m_jitCacheCompileNanoseconds`. When measuring, `BaselineJITPlan::compileInThreadImpl(JITCompilationEffort)` reads the thread CPU clock after `updateAllLazyValueProfilePredictions` and after the `Safepoint` block, and stores the difference.
- When measuring, `BaselineJITPlan::finalize` reads the clock at entry and right before the finalize capture hook; the difference, minus what the relink timer added meanwhile, is the finalization without relinking. It calls `takeRelinkNanoseconds` at entry, discarding what relinks outside it left, and again right before the hook, so it subtracts only its own relinks.
- `ScriptExecutable::installCode` wraps `oldCodeBlock->unlinkOrUpgradeIncomingCalls(vm, genericCodeBlock)` in a `JITCache::RelinkTimer`, a scope that, when a bench report is open, adds the call's thread CPU time to the report's relink accumulator. `installCode` also runs during a collection, when `CodeBlock::jettison` reinstalls a jettisoned CB's alternative for `ScriptExecutable::jettisonCodeBlockEdgeIfDead` (`JettisonDueToWeakReference`, `JettisonDueToOldAge`) in the End phase, on whichever thread drives it. The timer measures only when `!vm.heap.currentThreadIsDoingGCWork()`, so it touches the report only on the VM thread outside GC work, and no collection's relink reaches a `finalize`.
- THREAD's installation bound has both sides run with per-VM support already generated. `JITThunks` gains the private `std::atomic<uint64_t> m_supportGenerations` and two public members, since one of the sites that count lies outside the class: `void noteSupportGeneration()`, a relaxed `fetch_add` of one, and `uint64_t supportGenerations() const`, a relaxed load, enough since each reader compares two reads on its own thread. `JITThunks::ctiStubImpl` calls `noteSupportGeneration()` before it calls `generateThunk` on a miss, `JITThunks::lazyCommonThunk` inside its `NotGenerated` branch, and `InlineCacheCompiler::generateSlowPathHandler` (`bytecode/InlineCacheHandler.cpp`), through `vm.jitStubs`, before it creates a handler the VM lacked. When measuring, `compileInThreadImpl` reads the count at both ends of its span and records a change in a third member, `bool m_jitCacheSupportGenerated`; `finalize` reads it at both ends of its own span, and the install function at its start and its return (SPEC-integrator.md section 7.2). A change sets `supportGenerated` in the `compile` or `install` event, and IB3 sets that body aside. A compilation on a worker can also see support the VM thread generated meanwhile, which only sets aside one more body.

`BaselineJITPlan::finalize` passes both values and the support flag to the finalize capture hook in a `BaselineCompileTiming`, and the hook's step 1 writes them in the `compile` event (SPEC-integrator.md section 8.5), so the bench can set each import's cost beside its native cost.

## 10. Per-body event counts

- N21. With `useGC` off, `Heap::notifyIsSafeToCollect`, `collect`, `collectNow`, `collectAsync`, `collectSync` and `sweepSynchronously` return at once, so the heap never collects.
- N22. Every handler `llintOp` defines starts, in each of its three widths, with `traceExecution()` (`commonOp` in `llint/LowLevelInterpreter.asm`), and the call opcodes are among them (`commonCallOp`). `op_enter` and `op_catch` call it themselves; the wide prefixes and the three raw labels of `op_call_direct_eval`, whose handlers go straight to `slowPathForCommonCall`, do not. With `LLINT_TRACING`, which `llint/LLIntCommon.h` sets to 0, the macro calls the C slow path `_llint_trace` at that point, so no caller-saved register is live there. `llint/LLIntOfflineAsmConfig.h` turns build switches into offlineasm settings, as `OFFLINE_ASM_TRACING` and `OFFLINE_ASM_JIT_CAGE`, and `checkSwitchToJIT` adds to a field of the UCB through `CodeBlock::m_unlinkedCode`.
- N23. `VM::logEvent`, behind `CODEBLOCK_LOG_EVENT`, returns unless `m_perBytecodeProfiler` exists, which only `useProfiler` creates.
- N24. `handleExitCounts` (`dfg/DFGOSRExitCompilerCommon.cpp`), which DFG and FTL exit compilation call, returns for `ExceptionCheck` and `GenericUnwind` exits (`exitKindMayJettison`) and otherwise emits increments of `exit.m_count` and of the optimized CB's `m_osrExitCounter`. Under `printEachOSRExit` the exit code also calls `operationDebugPrintSpeculationFailure`, which prints `Speculation failure in <CB> @ exit #<n> (<bytecode index>, <kind>)` at each exit. `CodeBlock::jettison` prints `Jettisoning <CB>`, with ` and counting reoptimization` in `CountReoptimization` mode, before its two early returns for a CB already invalidated. After them it prints `Did invalidate <CB>` and, in that mode, calls `baselineAlternative()->countReoptimization()` and prints `Did count reoptimization for <CB>`. `jettison` prints these lines only while `DFG::shouldDumpDisassembly()` holds, which `dumpDisassembly` makes true. A collection's End phase jettisons too.
- N25. The DFG prints its code header, `Generated DFG JIT code for <CB>`, on the compiling thread while `SpeculativeJIT::compile` or `compileFunction` links (`JITCompiler::disassemble`), and the FTL prints its code in `FTL::compile` and `FTL::link`, all before `DFG::Plan::finalize`, which can still return `CompilationFailed` or `CompilationInvalidated`. `Plan::finalize` hands its result to `m_callback->compilationDidComplete` (SPEC-integrator.md N9), and the plan's callback is one of the three subclasses of `DeferredCompilationCallback`: `JITToDFGDeferredCompilationCallback` for a DFG plan, `ToFTLDeferredCompilationCallback` and `ToFTLForOSREntryDeferredCompilationCallback` for an FTL plan. Under `verboseOSR` each one's `compilationDidComplete` first prints `Optimizing compilation of <CB> result: <result>`, the two FTL ones with ` (for <DFG CB>)` before `result:`.

HARNESS.md's skipped-warm-up observable reads how much work the engine did for each body: bytecodes the LLInt executes, baseline and optimizing compiles, OSR exits, jettisons and reoptimizations. Twins builds count each event once, where the engine performs it. The engine keeps no such count per body: its event log (`CODEBLOCK_LOG_EVENT`) feeds only the per-bytecode profiler (N23), and THREAD fixes `useProfiler` off. Plain builds count nothing. ([history](SPEC-integrator-history.md#per-body-event-counts))

### 10.1 Counters

A body here is an `UnlinkedCodeBlock`, one specialization of one body's bytecode, which every CB of the body reaches through `CodeBlock::m_unlinkedCode` in every tier. In twins builds `bytecode/UnlinkedCodeBlock.h` declares

```cpp
#if ENABLE(JITCACHE_TWINS)
namespace JSC::JITCache {
struct BodyEventCounts {
    uint64_t llintInstructions { 0 };   // offset 0, where the LLInt adds
    uint64_t baselineCompiles { 0 };
    uint64_t dfgCompiles { 0 };
    uint64_t ftlCompiles { 0 };
    uint64_t osrExits { 0 };
    uint64_t jettisons { 0 };
    uint64_t reoptimizations { 0 };
};
}
#endif
```

and `UnlinkedCodeBlock` gains `JITCache::BodyEventCounts m_jitCacheEventCounts` as its last data member, with the accessor `jitCacheEventCounts()` (SPEC-integrator.md M10). Every UCB starts at zero, whether generated, decoded or imported, and no section carries the counts: they say what this process did.

| counter | where the engine adds one | thread |
|---|---|---|
| `llintInstructions` | a new macro `countJITCacheInstruction()` (`llint/LowLevelInterpreter.asm`), which, under the new offlineasm setting `JITCACHE_TWINS` (`OFFLINE_ASM_JITCACHE_TWINS` in `llint/LLIntOfflineAsmConfig.h`, from `ENABLE(JITCACHE_TWINS)`), loads `CodeBlock[cfr]` and then its `CodeBlock::m_unlinkedCode` into `t0` and adds one at `UnlinkedCodeBlock::m_jitCacheEventCounts`, as `checkSwitchToJIT` adds to `m_llintExecuteCounter`. `traceExecution()` calls it, and so it runs at the start of every handler `llintOp` defines, in each width, and where `op_enter` and `op_catch` trace themselves; the three `call_direct_eval` labels, which the native tracer misses, call it first (N22). Each instruction the LLInt begins thus counts once. No caller-saved register is live at these points, where the native tracer, or the slow path that `call_direct_eval` calls next, makes a C call. | the VM thread, running JS |
| `baselineCompiles` | `didFinalizeBaselineCompilation` (SPEC-integrator.md section 8.5), which `BaselineJITPlan::finalize` calls once a baseline compilation has installed its code, whatever the VM's JITCache state | the VM thread |
| `dfgCompiles`, `ftlCompiles` | `DFG::Plan::finalize` (`dfg/DFGPlan.cpp`), beside the plan-site fault call of SPEC-integrator.md section 9: one for the plan's tier (`isFTL()`, FTL OSR-entry plans included) when the result is `CompilationSuccessful` | the VM thread, with GC deferred (SPEC-integrator.md N8) |
| `osrExits` | `handleExitCounts` (`dfg/DFGOSRExitCompilerCommon.cpp`), which DFG and FTL exit compilation call: right after its increment of `exit.m_count` it emits `add64(TrustedImm32(1), AbsoluteAddress(...))` at the counter of `jit.codeBlock()->unlinkedCodeBlock()`. Each execution of an exit counts once against the body whose optimized code it leaves, for every exit kind that may jettison, which excludes `ExceptionCheck` and `GenericUnwind` (N24). The optimized CB keeps its UCB alive as long as its exit code exists. | compiled on the VM thread at the exit's first execution; the emitted code runs on the VM thread |
| `jettisons`, `reoptimizations` | `CodeBlock::jettison` (`bytecode/CodeBlock.cpp`): `jettisons` past the early returns for a CB already invalidated, right before the test that prints `Did invalidate`, and `reoptimizations` beside `baselineAlternative()->countReoptimization()` (N24) | the VM thread, or a collection's End phase on the thread that drives it, with the world stopped |

Every writer of a UCB's counters runs on its VM's thread or while that thread is stopped, and every reader below runs on the VM thread, so the counters are plain integers and take no lock.

Each count is exact. It is deterministic when its events are: a run with concurrent JIT off, which every twins run gets unless its options set it (section 7.4), compiles at fixed points of the script, and a test asserts only the counts its case keeps clear of when the collector runs and when a lease expires (HARNESS.md, Tests).

### 10.2 Reading counts in a script

`jitcacheBodyEvents(fn, kind)` (section 5.2) returns `{ llintInstructions, baselineCompiles, dfgCompiles, ftlCompiles, osrExits, jettisons, reoptimizations }` for the UCB of `fn`'s current CB of `kind` (`"call"` or `"construct"`), and `null` when the function has no CB of that kind; the dump of section 10.3 still reports a UCB that outlived its CBs. The function does not fall back to the UCB the function's `UnlinkedFunctionExecutable` holds, because those slots are private and the public accessor would decode, generate or import one. It throws a `TypeError` unless `fn` is a `JSFunction` with a `FunctionExecutable` and `kind` names a specialization. A UCB that replaced an earlier one, after Bun deleted code for instance, counts from zero.

A test reads counts in any role, `Off` included, so a case states what it expects against the JITCache-off run and against the producer's warmed run. A failed assertion throws and fails the run on its exit code. The oracle compares outputs (section 7.6), so a script prints no count that depends on its role.

### 10.3 The body-event dump

A bench's workloads cannot call `jitcacheBodyEvents`, so the hosts write the counts at the end of a run through `writeBodyEvents(VM&, const String& path)` (`JITCacheTwinsHost.h`): the jsc shell to the path `--jitcache-body-events` names, after `delta` and before the heap description, whose full collection would retire live UCBs (SPEC-integrator.md section 11.1), and Bun to the path its `--jitcache-body-events` names, in `Bun__JITCache__atExit`, after `delta`, each worker VM at that path suffixed with `.<executionContextId>` (SPEC-integrator.md section 11.2). The dump needs no JITCache configuration.

The dump is JSON lines. Walking the live cells under a `HeapIterationScope`, it writes `{"key":"<lowercase hex>", <the seven counts>}` for each UCB that has a key the UCB lane recorded (`UCBRegistry::keyOf`) and a nonzero count, then one `{"total":{<the seven counts>}}`, the sum over every live UCB and every UCB the VM destroyed before the walk. A dying UCB retires its counts: in twins builds `UnlinkedCodeBlock::~UnlinkedCodeBlock` calls `JITCache::retireBodyEventCounts(*this)` after the UCB lane's destructor hook (SPEC-integrator.md M10), which adds them to the VM's retired totals. A UCB that a collection found dead is destroyed only when it is swept, and the walk skips it before then, because `HeapIterationScope` stops allocation without sweeping and `forEachLiveCell` skips a cell the mark bits show dead. So the dump sweeps first, with two calls that start no collection. `vm.heap.stopIfNecessary()` runs the epilogue of a collection the collector thread ended, which waits for the VM thread's next stop point; that epilogue sweeps the precise allocations (`sweepEagerlyInEpilogue`), which hold up to eight cells of each UCB subspace (`JSCell::numberOfLowerTierPreciseCells`). `vm.heap.sweepSynchronously()` then sweeps every block. With `useGC` off no UCB is dead, and `sweepSynchronously` returns at once (N21). `JITCacheBodyEvents.h` declares

```cpp
#if ENABLE(JITCACHE_TWINS)
namespace JSC::JITCache {
void retireBodyEventCounts(const UnlinkedCodeBlock&);   // the thread sweeping the UCB's VM: adds its counts to the VM's totals
BodyEventCounts retiredBodyEvents(VM&);                 // VM thread: the totals so far
void forgetRetiredBodyEvents(VM&);                      // didFinalizeHeap
}
#endif
```

and `JITCacheBodyEvents.cpp` keeps the totals in a process-wide map from VM to counts under a leaf lock, because a UCB dies on whichever thread sweeps its VM's heap. `didFinalizeHeap` erases the VM's entry (SPEC-integrator.md section 4.6). Keys exist only in a VM that `start` configured, so a JITCache-off run's dump holds the total alone.

Only twins builds count, so the bench reads counts from twins-build runs of its workloads and times from release runs (SPEC-integrator.md IB11). A dump that cannot be written prints the path and `strerror(errno)` to stderr and calls `exit(1)`, as section 4's functions do, and the runner fails the run on its exit code.

## 11. The pin comparison

The comparison checks that the off path emits the pin's code, as HARNESS.md requires and THREAD Capture's "Other VMs emit unchanged" implies, except at the two native fixes THREAD Caches names (SPEC-image.md I4). It runs each jsc-hosted script of the corpus with this build and with a build of the pin, both with JITCache off, and compares each body's baseline code instruction by instruction, with every address replaced by what it names and the effects of the random draws removed. The pin cannot be instrumented, or it would no longer be the pin, so both builds describe their code only through what the engine prints natively, and the comparison names the addresses outside the engine. ([history](SPEC-integrator-history.md#the-pin-comparison-names-addresses-itself))

- N18. The JIT disassembler is compiled only where `ENABLE(DISASSEMBLER)` holds, which `wtf/PlatformEnable.h` turns off unless `BUN_ENABLE_JIT_DISASSEMBLER`, whose default is `ASSERT_ENABLED`, so debug builds have it and release builds print code ranges instead. Operation labels come only from `JITOperationList::addDisassemblyLabels`, which `dladdr`s each operation under `ENABLE(JIT_OPERATION_DISASSEMBLY)`, defined only for `USE(APPLE_INTERNAL_SDK)` on `CPU(ARM64E)`, so neither Linux target has them. Thunk labels come from `LinkBuffer::finalizeCodeWithDisassemblyImpl`, which registers `thunk: <name>` for each thunk it finalizes while `dumpDisassembly` or `logJIT` is on. x86_64's `tryToDisassemble` (`disassembler/X86Disassembler.cpp`, Zydis) never calls `labelFor` and prints absolute targets and immediates; ARM64's `A64DOpcode` prints a target inside the dumped code as its offset and another as its label, or `JIT PC`, `LLInt PC` or `<unknown>`.
- N19. `JIT::link` prints the baseline dump (`JITDisassembler::dump`) before `FINALIZE_BASELINE_CODE`, whose `LinkBuffer::performFinalization` runs the link tasks. `Call::linkThunk` and `Jump::linkThunk` (`assembler/AbstractMacroAssembler.h`) record an ARM64 thunk link in the assembler, which the copy into executable memory resolves, and add a link task on x86_64, so x86_64's dump shows its calls and jumps to thunks before they are linked. Under `useJITDump`, `LinkBuffer::finalizeCodeWithoutDisassemblyImpl` passes each finalized allocation's code, its address and size, to `PerfLog::log` (`logJITCodeForJITDump`), except from a `LinkBuffer` that rewrites code in place and owns no memory (`m_isRewriting`). `PerfLog::log` (`assembler/PerfLog.cpp`) dispatches the copy to `ProfilerSupport`'s serial `WorkQueue`, "JSC PerfLog", which reads the bytes at that address when it runs and writes them with the allocation's name as a perf `JIT_CODE_LOAD` record to `jit-<tid>-<pid>.dump` in `jitDumpDirectory`. The engine goes on running meanwhile, so a record can hold a rewrite made after its allocation was finalized. `ProfilerSupport`'s constructor registers an `atexit` handler that calls `barrierSync`, which waits for the queue, so every record is in the file once the process has exited (`runtime/ProfilerSupport.cpp`).
- N20. `AbstractMacroAssemblerBase::initializeRandom` seeds each assembler's generator from a process-wide counter that starts at one `cryptographicallyRandomNumber`, which `RandomDevice` reads from `/dev/urandom`, and no option fixes it. `BinarySwitch` seeds its shuffle from a process-wide counter of its own (`jit/BinarySwitch.cpp`), which starts at 0 and counts every switch that any tier or IC compiler builds.

What the engine prints names little on Linux (N18), so the comparison resolves names itself, from the runs' allocation headers, the builds' symbol tables and the order in which a run first references each object.

### 11.1 The pin build

The pin is webkitbun at the revision skills/SKILL.md names, `oven-sh/WebKit` `2e2aa2290fac856d6f451ceacb58f7f5b44dd057`, which this repository's history contains. Its source is a detached worktree of this repository at that commit, made once with `git worktree add --detach <path> 2e2aa2290fac856d6f451ceacb58f7f5b44dd057`, at `JITCACHE_PIN_SOURCE` (default `<build-root>/webkit-pin`). In this repository's `build.ts`, the `pin` target builds it with the profile and ninja target of `debug` (`debug-local`, `WebKit`) into `<build-root>/linux-<arch>-debug-local-pin`, passing the source as `BUN_WEBKIT_PATH` and that directory as `--build-dir`, after checking that the source's `HEAD` is exactly the pin and that `git diff --quiet HEAD` finds no change to a tracked file. The comparison reads the disassembler's output, so both builds are debug builds: the pin in `debug-local`, this build in `debug-local` or `debug-local-twins`, on the same machine, so both see one CPU feature vector. ARM64's pin is built the way this build's aarch64 build is and runs under the same emulation.

### 11.2 Runs

When the runner has `--pin` (section 7.1), it runs the comparison after each sequence's oracle (section 7.6), for every jsc-hosted script that does not declare `jitcache-pin: off <reason>` (section 7.2). For each option set j of the oracle's `Off` runs, it starts two runs, one with this build's `jsc` and one with the pin's, each the oracle run's command line without `--jitcache-describe-heap`, followed by

```text
--useConcurrentJIT=false --useGC=false --dumpDisassembly=true --logJIT=true
--useJITDump=true --jitDumpDirectory=<scratch>/pin<j>.<this|pin>
```

last, so they override the run's own options, and this build's run then takes `--pin-options` (section 7.1). The dump and log options leave the emitted bytes unchanged (options.md). The other two change when things happen, not what a compilation emits. Concurrent JIT off compiles every body at a fixed point of the script. With `useGC` off, `notifyIsSafeToCollect` and every collection request return early (N21), so nothing the code references dies or moves during the run, and each address names one object from the first time a body references it to the end.

Each run's standard error holds, in finalization order: the header `logJIT` prints for every finalized allocation, `Generated JIT code for <name>: [<start>, <end>) <size> bytes`; the per-bytecode dump of each baseline compilation (`JITDisassembler::dump`: a header naming the CB, the prologue, one block per bytecode headed by its index and opcode, `(End Of Main Path)`, the slow-path blocks marked `(S)`, `(End Of Slow Path)` and the tail); and the disassembly of every MathIC snippet and every MathIC inline rewrite, which `JITMathIC::generateOutOfLine` finalizes with `LinkBuffer`s of their own. The JIT dump file holds a record of every finalized allocation except an inline rewrite, with the bytes the profiler's queue copied, which a later inline rewrite may already have changed (N19).

### 11.3 Canonical code

The comparison tool, `Tools/Scripts/jitcache-pin-compare.ts`, turns each run's output into canonical code, walking the run's allocations in finalization order.

Allocations. Each header gives an allocation's range and name. The name with every hexadecimal number replaced by `0x?`, together with its ordinal among the run's allocations of that name, is the allocation's identity across runs. A baseline compilation's code is the region its dump covers. A MathIC snippet belongs to the body its jumps return into, at the bytecode block they return to, and an inline rewrite to the body and block that contain it. An inline rewrite is the allocation whose header starts inside the code of an earlier baseline body. Its `LinkBuffer` writes into that body and owns no memory, so its header gives the empty range `[p, p)`, and its code runs from p to the end of the one jump its listing shows, which `jumpThunk` emits as a 5-byte near `jmp` on x86_64 and a 4-byte `b` on ARM64.

Targets. The disassembler prints every PC-relative target as an address, but on x86_64 the baseline dump runs before finalization, whose link tasks write the calls and jumps to thunks (N19). On x86_64 the tool therefore reads the displacement of every `call`, `jmp` and conditional jump with a 32-bit displacement from the JIT dump's bytes at that instruction, in the record of the body's own allocation, except for a branch whose displacement field overlaps an inline rewrite of that body. The queue copies the body's bytes before the rewrite or after it (N19), so the tool keeps the disassembly's target for such a branch. That target is the linked one: the dump printed the body before any rewrite, and a MathIC inline region jumps only within its own body. ([history](SPEC-integrator-history.md#the-pin-comparison-names-addresses-itself))

Instructions. Each instruction keeps its mnemonic and operands as the disassembler printed them, without its address, its annotation and its comment. Then, within each straight-line run, which ends at every branch, before every branch target and at every block boundary, since a bytecode's first instruction is the target of switch tables, exception handlers and OSR entries, which no branch in the code names:

1. Every `nop` is dropped, whatever its encoding: the entry's random `nop`, the random padding a blinded operation emits when it has no scratch register (`MacroAssembler::mul32`, `compare64`), and alignment.
2. Constants fold. An instruction that sets a register from immediates alone (`mov` and `movabs` of an immediate, `movz`, `movn`, `orr` with the zero register), or from that register's known constant and an immediate (`movk`, `xor`, `add`, `sub`, `and`, `or`, `rol`, `ror`, at the operation's width), is dropped, and the register's constant becomes known. The next instruction that reads the register gets the constant in its place, `$c` as an operand and the absolute `[c + d]` as a memory base. For the temporaries the assembler reserves, `r11` on x86_64 and `x16` and `x17` on ARM64, the constant stays known until something writes the register, and a temporary whose run ends with its constant unread gets a canonical `set <register>, <c>` there. ARM64's two, which the assembler caches, keep their constants across the fall-through of a conditional branch too, since only a `Label` and `Jump::link` invalidate them. For any other register, a canonical `set <register>, <c>` goes before that reader, or before the end of the run when nothing reads it, and the knowledge ends: a VM that emits a constant unfolded through such a register, as only recording may, then differs from the pin. This removes x86_64's xor and rotation blinding of moves and of operands passed through `r11`, and ARM64's value-dependent materialization lengths and cached-temporary reuse.
3. Split immediates merge. Two consecutive instructions that apply `add`, `sub`, `and`, `or` or `xor` with an immediate to one register become one with the combined immediate, and `lea a(%s), %d` followed by `add $b, %d` becomes `lea (a+b)(%s), %d`. This removes x86_64's addition, and and or blinding of live registers.
4. On ARM64, a conditional branch over a single `b T` becomes the inverted conditional branch to `T`. Compaction measured the distance to the thunk, which differs between processes.

Rules 2 and 3 fold blinded moves, operands passed through a temporary and split immediates. A blinded form that gives a register another role needs a rule of its own, and the tool has one for each form the corpus reaches, which H7's forced blinding (section 11.5) shows complete:

- x86_64: when `branchAdd32`, `and32`, `or32` or `xor32` build the constant in the destination, `<d = c>; op %s, %d` becomes the plain `mov %s, %d; op $c, %d`, and `branchMul32`'s `<d = c>; imul %s, %d` becomes `imul $c, %s, %d`. A zeroing `xor %r, %r` sets the register to 0, since a blinded `move(Imm32)` whose key equals the value emits it. Baseline code blinds `compare32` through `r11`, since it passes the left operand as the destination, so rule 2 folds it.
- ARM64: an access at a constant address is written as `ldr` or `str` at `[c + d]`, whichever encoding reached it, since the cached memory temporary reaches a field with `ldur` at an offset where a fresh materialization uses `ldr`.

Names. Every PC-relative target, every immediate operand, every constant of rule 2 and every absolute address operand below 2^48, the top of user space on both architectures, is replaced by what it names; immediates are named as constants are, so a blinded form and the plain form it folds back to name alike:

| the value lies | its name |
|---|---|
| in the same body's code | the block (prologue, bytecode index and path, or tail) and the ordinal of the canonical instruction there |
| in another allocation | that allocation's identity and the offset |
| in the build's `jsc`, whose engine sits at its link-time address (SPEC-integrator.md N15) | the symbol at or below it in that executable's symbol table (`llvm-nm --defined-only --demangle`) and the offset |
| anywhere else from 2^32 up: the VM and its fields, cells, atoms, a UCB's arithmetic profiles, an artifact's MathICs and switch tables, the structure base | `h<n>`, numbered in the order the walk first meets the value |

A value below 2^32 that lies in no allocation and outside the engine stays literal: no heap, VM, executable-pool or structure-reservation object lies below 4 GiB on either architecture, natively or under QEMU, while the engine, which is not position-independent, does. So do values from 2^48 up, such as JSValue tags and encoded numbers. Since nothing dies during the run, two operands get one `h` number exactly when they name one object, and the numbers agree between the two runs exactly when both reference the same objects in the same order. A body that uses another VM field than the pin's at one site shows a different number at that site or at the next use of either field, which covers every field the corpus uses more than once. Two operands match when their names match. An integer constant that lies inside either build's loaded `jsc` segments also matches an equal literal, since each build puts another symbol at the same address; branch targets, allocation identities and `h` numbers match by name alone. The main-path blocks of `super_construct` and `super_construct_varargs`, which section 11.4 exempts, assign no `h` number, so the native fix's own values cannot shift the numbering of later blocks. ([history](SPEC-integrator-history.md#the-pin-comparison-names-addresses-itself))

### 11.4 What it compares

For the two runs of an option set, in order:

1. Both end with the same status and print the same standard output: with JITCache off, this build behaves as the pin.
2. Both finalize the same allocations in the same order, by identity. A body's code depends on what ran before it, through its profiles and through the shuffle that `BinarySwitch` seeds from a process-wide counter (N20), so the comparison is exact only while the two runs compile alike, and the first divergence is reported as such. The two native fixes can change optimizing compiles, since the `super_construct` cache changes what the DFG freezes and the `negate` fix what its profile holds after a regeneration; a script whose optimizing compiles they change declares `jitcache-pin: off` and says so.
3. Each baseline body, each of its snippets and each of its inline rewrites has the same canonical instructions, block by block, except the main-path blocks of `super_construct` and `super_construct_varargs`, which the ICs lane's fix of the stored callee changes (SPEC-ics.md E1). The `negate` fix changes only the operation a regeneration repoints the slow call to, which no dump prints (SPEC-image.md section 6.4), so it needs no exemption.

The first difference fails the sequence and names the option set, the body by ordinal and identity, the block and both canonical instructions. Output the tool cannot read, such as a header it cannot parse, a baseline body without its record in the JIT dump or an operand it cannot classify, fails the sequence and names what it could not read.

### 11.5 Forced blinding

Blinding happens at random, to one candidate immediate in 64, so a blinded form that rules 2 and 3 do not fold would fail a comparison only now and then. Twins builds make it deterministic: `MacroAssembler::shouldConsiderBlinding()` (`assembler/MacroAssembler.h`) returns true without drawing while `JITCache::forcesBlindingForTesting()` holds, a function that header declares and `JITCacheTwinsHarness.cpp` defines, set by `setForcesBlindingForTesting(bool)` before `JSC::initialize` from the shell flag `--jitcache-test-force-blinding` (section 5.1). Every immediate that passes the assembler's value tests is then blinded. The thread that emits, the VM thread or a JIT worker, reads the flag without a lock, since it is written before any VM exists. ARM64 never blinds, so the flag changes nothing there. Only runs with JITCache off take it: in a producer it would blind literals that the consumer's image twin, which replays the producer's draws without the flag, emits plain (SPEC-image.md section 11.3). H7 compares a run with the flag against one without it, so every blinded form the corpus reaches has to fold back.

## 12. Producer kills

A producer can die at any point of a commit, and THREAD Capture and Failures say what must survive. Only the writer's system calls change the files, so a commit's points are the states those calls leave, and twins builds can kill the producer at each of them (container sub-SPEC section 8.3). The flag `--jitcache-test-kill=<point>@<n>` (section 5.1) makes the writer's n-th commit call `raise(SIGKILL)` at the point, so no signal handler, destructor or exit hook runs: ([history](SPEC-integrator-history.md#producer-kills))

| point | after, in container sub-SPEC section 8.2 | the files |
|---|---|---|
| `before-create` | step 2, the layout | no temporary; the key's earlier body, if any |
| `after-create` | step 3 | an empty temporary |
| `mid-stream` | step 4's first `write` | a temporary holding at most that write's bytes |
| `after-stream` | step 4 | a temporary with every section and no envelope |
| `after-envelope` | step 5 | a complete temporary, not reread |
| `after-reread` | step 6 | a validated temporary |
| `after-rename` | step 7 | the new body, published, with the epoch not bumped |

A kill at one of the first six points leaves the commit undone: every body committed before it stays importable, the key keeps no body or its earlier version, and the next `clean` reports one temporary, or none for `before-create`. No reader sees a temporary, since it sits in `cache/` and readers open only `bodies/`. A kill after the rename finds the body already committed: a later process lists the directory at its `start` (container sub-SPEC section 6.2) and imports the body, and no temporary remains. The epoch the kill left unbumped delays only processes already running, which see the body at the next bump (container sub-SPEC section 6.3).

A test passes the flag among a producing run's options, declares `// jitcache-expect-exit: <i> kill` for that run (section 7.2), and follows it with a `Maintenance clean $ARTIFACT` run and a consumer, as `producer-kill.js` does (SPEC-integrator.md section 15.2). A run that does not end by `SIGKILL`, because it made fewer than n commits or failed in another way, fails its exit check. Bun-hosted runs have no equivalent (section 7.7).

## 13. Tests of the harness

- H1. Runner self-tests in `JSTests/jitcache/integrator/runner/`: a script whose consumer prints differently from its `Off` run fails, and so does one whose producer, or whose ConsumerProducer followed by a consumer, does; one whose consumer leaves a different value reachable from a global binding fails, which also shows that its oracle run wrote `oracle<j>.heap` rather than the consumer's file, and passes when it declares `jitcache-heap: off`; a run with `--jitcache-test-twin-entry=difference` (section 5.1) fails; a run with `--jitcache-test-twin-entry=skip` fails when every JITCache run of its sequence up to it has `--useConcurrentJIT=false`, and passes when that run or an earlier one has `--useConcurrentJIT=true`; `--jitcache-test-twin-entry=coincidence:heap` repeats the sequence once, and `coincidence:executable-pool` or `coincidence:engine-image` fails at once; `coincidence:executable-pool` passes when `jitcache-expect-twin` declares it, and a declared report that never appears fails the run; a directive written `1:0` applies to run 0 of the second sequence only; and each run receives its sequence index as `arguments[3]`; `jitcache-expect-exit` accepts an abort; a Producer whose production a limit of one page ends fails without `jitcache-expect-fault: 0 budget.limit`, passes with it, and fails when the directive names another step; a Consumer with no body to install fails, and passes with `jitcache-expect-no-install: 1`; with `--extra-options=--jitcache-test-fixed-heap-probes` the runner calibrates twice, the second time with `WebKitMallocForceEnabled=1`, and stops before any sequence with an error naming the heap domain.
- H2. Placement: across 50 producer-then-consumer sequences on each architecture, run in the environment the runner's calibration chose, every consumer's recorded pool range is disjoint from its producer's, its structure base differs, and each of its three heap probes differs from its producer's.
- H3. `testjitcache --list` lists every test with its options; a test registered with options reads each of them back from `Options` (`--useConcurrentJIT=false --numberOfGCMarkers=1`, and a group whose `--useJIT=false` leaves `useBaselineJIT` off once `notifyOptionsChanged` has run); a failing check prints its file, line and message, and the process exits with 1; an unknown option in a group makes it exit with 2.
- H4. The twin report's lines parse as JSON, and a line written just before an `abort()` is in the file.
- H5. The reachable-heap description, in `JSTests/jitcache/integrator/runner/`: two `Off` runs of each integrator JS test write identical end-of-run descriptions. Each of these differences between two otherwise equal scripts changes the end-of-run description: one reachable value, one property attribute, the order in which two properties were added, one prototype, one closure variable, one `Map` entry, one weak-map entry whose key stays reachable, one private field, one module binding and one global `let`; the first eight also change what `jitcacheDescribeHeap` gives of an object that reaches them. A value that only the shell's `arguments` reaches changes neither. Each of these pairs, alike to JavaScript, describes alike: a dictionary object and an ordinary one with the same properties added in the same order; an array of the same values with `Int32`, `Double` and `Contiguous` storage; a rope and a flat string; a `WeakRef` whose target only it reaches, collected or not; a function whose body ran in the LLInt and one whose body ran in baseline code; `Math`, read before the end of the run or never. A value only the global object reaches changes no description of explicit roots, while a default-root description shows it.
- H6. Event counts (section 10), in `JSTests/jitcache/integrator/runner/`, in `Off`-only sequences, which no oracle compares, so the scripts print their counts. With `--useBaselineJIT=false`, a function without branches gains, at each of three calls, as many `llintInstructions` as its bytecode has instructions, which a checker counts in the dump `$vm.dumpBytecodeFor` prints, and so does one that calls `eval` directly; a function whose loop runs n times gains, over n = 0, 1 and 2, amounts in arithmetic progression; and every other count stays 0. With the baseline on, a function called until it compiles shows `baselineCompiles` 1, and its later calls add no LLInt instruction. A function driven through optimizing compiles, speculation exits, a jettison and a reoptimization, in a run with `--printEachOSRExit=true --dumpDisassembly=true --verboseOSR=true`, prints counts that a `jitcache-check` checker finds equal to what the engine printed for its body (N24 and N25), each line printed where section 10.1 counts: one `Speculation failure` line per exit of a kind that may jettison, one `result: CompilationSuccessful` line per optimizing compile, an FTL compile when the line carries ` (for <DFG CB>)` and a DFG compile otherwise, one `Did invalidate` line per jettison, and one `Did count reoptimization for` line per reoptimization. With `--jitcache-body-events` and `--useUnlinkedCodeBlockJettisoning=true`, a function that ran and was then made unreachable keeps its counts in the dump's total after more `fullGC()` calls than `UnlinkedCodeBlock::maxAge`, which age its UCB until its unlinked executable stops marking it; the run leaves `sweepSynchronously` off, so when the UCB sits in a block, its destruction can wait for the dump's own sweep (section 10.3). A Producer run's dump has a line for each keyed body still alive, with the counts `jitcacheBodyEvents` gave for it at the end.
- H7. The pin comparison (section 11). `JSTests/jitcache/integrator/runner/self-test.ts` runs H7 once on each architecture, given that architecture's twins build and its `debug-local` build: `bun JSTests/jitcache/integrator/runner/self-test.ts --build=<build-root>/linux-<arch>-debug-local-twins/deps/WebKit --plain-build=<build-root>/linux-<arch>-debug-local/deps/WebKit`, the builds that `bun build.ts twins` and `bun build.ts` make, with `--arch=aarch64` for ARM64, whose processes run under QEMU (section 7.9). Each build, compared with itself on the runner's fixtures and the integrator corpus, shows no difference. The twins build's comparison runs through the runner with `--pin` naming the build's own directory, and finds none either with `--pin-options=--jitcache-test-force-blinding` (section 11.5). In a plain build the runner skips every script of `integrator/`, since each requires twins (section 7.4), so `self-test.ts` runs the plain build's comparison itself: it calls the runner's `comparePinOptionSet` (`Tools/Scripts/run-jitcache-tests`) for every option set that section 11.2 would compare, with the plain build's `jsc` on both sides and section 11.2's command lines in plain mode. Without `--plain-build`, `self-test.ts` skips that half and prints `SKIP`, and H7 has not run in full. On a fixture that allocates objects, the twins build's comparison with `--pin-options=--forceGCSlowPaths=true` fails at the first block whose inline allocation became a jump to its slow path. `jitcache-pin-compare.ts`'s own tests, on crafted output pairs for both architectures: pairs that differ in an operation's symbol, an allocation's identity, an `h` number, an immediate, a displacement, a register, a mnemonic, the order of finalized allocations, the target of an inline rewrite, or a branch target the JIT dump gives where no inline rewrite overlaps it are reported; pairs that differ only in the addresses of the same objects, in `nop`s, in blinded forms, in ARM64 materialization lengths and temporary reuse, in the two forms of an ARM64 conditional branch to a thunk, or in whether the JIT dump copied a body before or after its inline rewrite compare equal, and so does a pair that differs only in a `super_construct` main path. A script whose pin run ends with another status or prints something else fails the sequence, and passes once it declares `jitcache-pin: off`.
- H8. Kills (section 12). For each point, a Producer with `--jitcache-test-kill=<point>@1` and two bodies to commit ends by `SIGKILL`, which `jitcache-expect-exit: 0 kill` accepts and `jitcache-expect-exit: 0 abort` rejects, and leaves one temporary in `cache/` for the five points from `after-create` to `after-reread` and none for the other two. With `@1000` the run exits 0, which `kill` rejects.
