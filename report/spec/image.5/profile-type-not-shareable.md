mechanical

SPEC-image.sites.md, part C, row C24 (`JIT::emit_op_profile_type`, `JIT::emit_op_profile_control_flow`): helper "none: each emitter clears `m_isShareable`, so `finishBaselineCompile` makes the record `Unrecordable(NotShareable)` (SPEC-image section 4.7); native". SPEC-image.md section 4.1 gives the same reason to `JIT::link` alone ("`NotShareable` | `JIT::link` finds `m_isShareable` false").

That holds for `profile_control_flow` but not for `profile_type`. Its template passes `TrustedImmPtr(&vm())` to `operationProcessTypeProfilerLog`, and under a recorder that argument reaches the guard of section 4.4 (`CCallHelpers::setupArgumentsImpl`, the `TrustedImm` overload, which calls `JITCache::notePointerArgument`). The guard marks the record `Unrecordable(UnannotatedPointerArgument)` during emission. The first reason is final (`ImageRecorder::markUnrecordable` returns once the recorder has stopped), so when `finishBaselineCompile` later finds `m_isShareable` false, the reason is already set. Such a body would report a census gap in its status diagnostic, which is not what happened.

The code keeps both templates' emission native and marks the record `Unrecordable(NotShareable)` in each emitter, right where it clears `m_isShareable` and before anything is emitted. That gives a `profile_type` body the reason the row states. `finishBaselineCompile` step 4 still marks it too, so a record left recording is still caught there. Nothing changes for other tasks. A `profile_control_flow` body (T11) ends `NotShareable` as before.

What the SPEC should record: row C24's helper column reads "under a recorder, `markUnrecordable(NotShareable)` where the emitter clears `m_isShareable`, then native", and section 4.1's `NotShareable` row names the two profiler emitters beside `JIT::link`.

Evidence: `JIT::emit_op_profile_type` and `JIT::emit_op_profile_control_flow` (`jit/JITOpcodes.cpp`); `CCallHelpers::setupArgumentsImpl` (`jit/CCallHelpers.h`); `JITCache::notePointerArgument` (`jitcache/ImageEmission.cpp`); `ImageRecorder::markUnrecordable` and `ImageRecorder::finishBaselineCompile` (`jitcache/ImageRecorder.cpp`).
