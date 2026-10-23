# gc.md: Bun's GC timer requests idle-tagged full collections once the heap goes quiet

Doc: `skills/reference/knowledge/common/gc.md`, Abstract, first paragraph.

## Passage

> When it names neither, as with the request that allocation triggers (`Heap::collectIfNecessaryOrDefer`) and the routine one from Bun's own GC timer (`GarbageCollectionController::perform_gc`), the edens before it choose between the two: [...]

## What changed

`GarbageCollectionController::perform_gc` (`~/bun/src/jsc/GarbageCollectionController.rs`) now takes `idle_full`. On a routine tick it still calls `collect_async(false)`, a request with no scope. `idle_tick` sums the tick intervals during which the heap did not grow by more than a slack (at least 2 MB, 256 KB per second of interval). When that quiet time crosses one of the thresholds from `BUN_IDLE_GC_SECONDS` (default `10,110,480`: 10 s, 2 min and 10 min of cumulative quiet; `0` turns it off; never with the inspector enabled), the tick calls `JSC__VM__collectAsyncIdle` instead. That requests a full collection with `GCRequest::isIdle` set and a `didFinishEndPhase` task that wakes the parked JS thread so the sweep can run (`src/jsc/bindings/bindings.cpp`). Before the last threshold, `idle_tick` first calls `JSC__VM__shrinkFootprintNow`, which runs `VM::shrinkFootprintNow` with `LeaveCollectionToCaller` and `KeepCodeInUse`; if that cannot run (a collection is under way), the tick's quiet is not counted and the next tick retries. Growth resets the quiet time.

On the engine side, `GCRequest::isIdle` (under `USE(BUN_JSC_ADDITIONS)`) is visible as `Heap::isIdleCollection()`. Only such a collection may let FTL code, and DFG code without a tier-up counter, age out (`CodeBlock::shouldJettisonDueToOldAge`, `agesByMutatorQuietness`). An idle request is never subsumed by a non-idle one (`GCRequest::subsumedBy`).

## Proposed text

Replace "and the routine one from Bun's own GC timer (`GarbageCollectionController::perform_gc`)" with "and the routine one from Bun's own GC timer (`GarbageCollectionController::perform_gc`), except that once the heap has stopped growing for 10 s, 2 min and 10 min of cumulative quiet (`BUN_IDLE_GC_SECONDS`) that timer requests a full collection tagged idle (`JSC__VM__collectAsyncIdle`, `GCRequest::isIdle`), the only kind in which optimized code without an execution counter may age out, and before the last one returns decoded code nobody links against to its bytecode payload (`JSC__VM__shrinkFootprintNow`)".
