---
name: cpp-coding-standards
description: C++ in WebKit — ownership, bounds, concurrency, JavaScriptCore GC and exceptions, style and verification
---

# WebKit C++

## Scope and authority

Apply to first-party WebKit C++. Follow the checkout's API contracts and build configuration. Resolve style ambiguities with checker tests; legacy examples are not exemptions.

`Source/cmake/OptionsCommon.cmake` and `Source/cmake/WebKitCompilerFlags.cmake` define the language level and compiler restrictions, not `.clang-format`. Do not introduce C++ exceptions or RTTI. Paths below are relative to WebKit.

## Ownership and lifetime

| Mechanism | Contract |
|---|---|
| `std::unique_ptr` | Exclusive ownership; use `makeUnique` / `makeUniqueArray` for supported types, not `std::make_unique`. |
| `Ref` / `RefPtr` | Non-null / nullable intrusive ownership; requires compatible `ref()` and `deref()`. |
| `CheckedRef` / `CheckedPtr` | Non-owning checked access; the pointee must outlive the handle. Stale handles can crash without dereferencing. |
| `WeakPtr` | Non-owning, null on destruction; check before use and account for intervening reentrancy. |
| Raw pointers, references, views | Borrowed; the owner and backing storage must remain alive and stable through the last use. |

- Follow each type's factory and allocator requirements. Checked/weak pointers require compatible pointees; GC cells have separate allocation and rooting mechanisms.
- Adopt new refcounted objects exactly once. Spell `Ref` / `RefPtr`, not `auto`, on adoption-initialized locals.
- Acquire resources into RAII owners immediately. Default copy/move only when it preserves invariants; otherwise implement or delete explicitly. Break refcounted ownership cycles explicitly.
- Retain refcounted objects across callouts that may drop their last owner. Name self-protection `protectedThis`; retaining an object does not preserve mutable state.
- Escaping captures need lifetime-safe ownership; copied pointers/views do not retain targets. Do not keep iterators or views across potentially invalidating container mutations.

Contracts: `Source/WTF/wtf/` — `Ref.h`, `RefPtr.h`, `CheckedPtr.h`, `WeakPtr.h`, `StdLibExtras.h`, `UniqueArray.h`; `Tools/Scripts/webkitpy/safer_cpp/checkers.py` for checked/refcounted members, arguments, locals and captures.

## Bounds, casts and failures

- Initialize members before use or publication, preferably with member/constructor initializers. Keep deliberately uninitialized storage inaccessible until filled.
- Use `CheckedSize` for fallible size arithmetic; check overflow before `.value()`, allocation, slicing or copying. Validate ranges without overflowing: `offset <= size && length <= size - offset`.
- Prefer `std::span` and bounded helpers to pointer arithmetic. Validate external lengths before assertion-based helpers such as `memcpySpan`; bounds checks do not establish storage lifetime.
- Validate representability before narrowing. Casts do not validate sizes, enum values or signed/unsigned conversions.
- `downcast<T>` asserts an established type invariant; `dynamicDowncast<T>` returns null on mismatch. Check that result rather than duplicating it with an `is<T>` precheck.
- Avoid C-style casts. Reinterpreting storage requires valid alignment, size, object lifetime and aliasing; a pointer cast supplies none of them.
- Return recoverable failures through existing result APIs. Reject malformed external input explicitly; assertions express internal invariants.
- Keep required effects outside assertions. `ASSERT` depends on `ASSERT_ENABLED`; use `ASSERT_UNUSED` for assertion-only values and `RELEASE_ASSERT` for invariants that must terminate on violation in every build.

Contracts: `Source/WTF/wtf/` — `CheckedArithmetic.h`, `StdLibExtras.h`, `TypeCasts.h`, `Assertions.h`; `Source/JavaScriptCore/runtime/JSCast.h`.

## Concurrency

- For locked state, use existing `Lock` / named `Locker` guards and thread annotations; cover every protected access. Unnamed guards expire at the statement boundary.
- Establish thread affinity, guarded state and lock order. Thread-safe reference counting does not protect mutable payloads.
- No unknown or reentrant callouts under locks without an explicit locking/reentrancy contract.
- Cross-thread captures must satisfy lifetime and thread-affinity contracts.
- Atomic publication must establish happens-before from initialization to use; keep the default `seq_cst` ordering. Prove pointee lifetime and reclamation separately. `volatile` provides no inter-thread synchronization.

Contracts: `Source/WTF/wtf/Locker.h`, `Source/WTF/wtf/Atomics.h`.

## JavaScriptCore, when applicable

- For values surviving possible collection, identify roots, traced owners or live conservatively scanned references. Local declarations do not guarantee compiler liveness; use `ensureStillAliveHere` where required.
- `Strong` supplies an independent root. `WriteBarrier` records stores for a GC owner; maintain barriered writes and visitor tracing. C++ ownership wrappers do not substitute for GC management.
- `MarkedArgumentBuffer` is a temporary rooted collection; handle `hasOverflowed()` after growth before consuming contents.
- Between `allocateCell()` and `finishCreation()`, allow neither safepoints nor publication to reachable objects. Extended initialization follows `ObjectInitializationScope`'s contract.
- `DeferGC` does not replace roots/barriers or exclude all concurrent GC work. Its destructor may collect: finish initialization and establish liveness before leaving the scope.
- Declare `DECLARE_THROW_SCOPE(vm)` before potentially throwing work. After potentially throwing calls, use `RETURN_IF_EXCEPTION` before continuing work that assumes success; it handles VM traps too.
- Deliberate caller propagation uses `scope.release()` / `RELEASE_AND_RETURN`; ensure the caller checks. `EXCEPTION_ASSERT` does not handle exceptions. Preserve termination exceptions when deliberately catching.

Contracts: `Source/JavaScriptCore/` — `runtime/JSCellInlines.h`, `runtime/MarkedVector.h`, `runtime/WriteBarrierInlines.h`, `heap/Strong.h`, `heap/DeferGCInlines.h`, `runtime/ThrowScope.h`, `runtime/ExceptionScope.h`.

## Style and verification

- Prefer WTF containers/strings: `Vector`, `HashMap`, `HashSet`, `String`, `StringBuilder`. Standard facilities such as `std::span` and `std::unique_ptr` remain valid.
- Use `_s` / `ASCIILiteral` where WTF string APIs expect them; choose `fromUTF8` / `fromLatin1` explicitly for encoded input. Do not suffix arbitrary literals.
- Move with `WTF::move`, not `std::move` or `WTFMove`.

| Area | Rule |
|---|---|
| Names | `UpperCamelCase` types/namespaces/enumerators; `lowerCamelCase` functions/variables, including acronym case. Full words; private class fields `m_`, static fields `s_`. |
| Interfaces | Descriptive verbs; booleans `is…` / `did…`; getters bare, setters `set…`, out-argument getters `get…`. Non-creating variants of creating getters use `IfExists`. |
| Parameters | Name ambiguous scalars; omit names repeating the type. Prefer scoped enums to literal boolean flags except self-explanatory setters. Required out-arguments use references; optional ones use pointers. |
| Classes | Single-argument constructors are `explicit` unless a fast, natural conversion. Base virtual declarations use `virtual`; overrides use `override` or `final`, never combined. |
| Values | Prefer `const` for immutable values and observing methods; typed constants/inline functions over macros. Use `nullptr`, direct truth/null tests, `unsigned` rather than `unsigned int`. |
| Headers | `#pragma once`; self-contained dependencies; no `config.h`. In `.cpp`, include `config.h` first, own header second, remaining includes sorted. |
| Namespaces | No namespace-scope using declarations/directives in headers except WTF's explicit end-of-header exports. Keep `std::` qualification; implementation using-directives belong inside namespaces. |
| Layout | Follow `.clang-format`. Omit braces for single-statement, single-line control bodies without comments; empty bodies use `{ }`. Omit empty lambda parameter lists where legal and redundant `else` after returning branches. |
| Comments | State constraints and invariants, not edit history. Sentence case and punctuation; `FIXME:` without attribution, not `TODO`. |

Run formatting/style checks from the checkout, scoped to changed C++ files: `clang-format` with its `.clang-format`, then `Tools/Scripts/check-webkit-style`. Diagnostic contracts live in `Tools/Scripts/webkitpy/style/checkers/cpp.py` and `cpp_unittest.py`. Investigate findings; do not mechanically change semantics or add suppressions.

For lifetime/cast changes, run `Tools/Scripts/analyze-safer-cpp` when its toolchain is available; consult `--help` for configuration. Compile/test affected configurations, including assertions-disabled builds where behavior could differ. Cover affected invalid-input, overflow, lifetime and exception paths. Report unavailable checks; formatting and static analysis do not prove correctness.
