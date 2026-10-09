mechanical

SPEC-integrator.md, section 3.2, step 0 of `start`: "The process runs on Linux; otherwise `Rejected` at `start.platform` (section 11)." Section 11 says the same: "Off Linux, `start` returns `Rejected` at `start.platform`".

THREAD's opening names a narrower platform: "the targets are Linux x86_64 and ARM64 ... on other platforms JITCache's code compiles and `start` returns rejected." A Linux build for another CPU, or one built without the JIT, passes the SPEC's test without being a target, and the code cannot serve it. On such a CPU `thisHeaderArchitecture()` is 0. A Producer would then write a header that every reader classes as corrupt, because container sub-SPEC section 3.1 allows only architecture 1 or 2. Without `ENABLE(JIT)`, manifest entry M4 calls `willDestroyVM` and `didFinalizeHeap` only under `ENABLE(JIT)`. A state that `start` created would never be destroyed, and its producer lock would outlive the VM.

`start` therefore rejects at `start.platform` unless `OS(LINUX) && ENABLE(JIT) && (CPU(X86_64) || CPU(ARM64))` holds, which is the platform THREAD names. On every target THREAD names, the step behaves as the SPEC writes it. The SPEC only has to record this condition for step 0 and section 11. Maintenance's own `platform` failure (maintenance sub-SPEC section 2) belongs to task 11, which may want the same condition.

Evidence: `JSC::JITCache::start`, and the `#if` around the `JITCacheAPIInternal` helpers that open the artifact, in `Source/JavaScriptCore/jitcache/JITCacheAPI.cpp`; `thisHeaderArchitecture` in `Source/JavaScriptCore/jitcache/JITCacheContainer.h`; the `#if ENABLE(JIT)` blocks around `JITCache::willDestroyVM` and `JITCache::didFinalizeHeap` in `VM::~VM`, `Source/JavaScriptCore/runtime/VM.cpp`.
