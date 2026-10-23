# bunpatches.md: recount of the opening PS and the Details table for the 1.4.3 pin

Doc: `skills/reference/knowledge/common/bunpatches.md`: the PS under the title, and the "Details" table.

## Passages

> PS: of the 2,152 commits that separate `WebKit/WebKit@0d58b764f3` from `oven-sh/WebKit@2e2aa2290f`, 333 are merges and rewritten old history, and more than half of the volume is `#include` swaps. In practice, what is left lives in `runtime/`, `heap/`, the bytecode cache and the new `ffi/` directory; the baseline JIT has a single guarded site, a new field in `BaselineJITCode`, and no emitter changed.

Details: "files changed in JSC + WTF + bmalloc | 1,191 (+39.5k / −6.2k lines)", "files with a change other than `#include` | 516", "`USE(BUN_JSC_ADDITIONS)` sites | 473, in 165 files", "per directory | `runtime/` 297 · WTF 31 · `ffi/` 30 · `dfg/` 24 · `heap/` 23 · `bytecode/` 11 · `parser/` 10 · `ftl/` 8 · `interpreter/` 7 · `bytecompiler/` 3 · `jit/` 1 · others 28 [...]".

## Measurements

The pins are upstream `WebKit/WebKit@dbdca7545d` and `oven-sh/WebKit@0c06faad`. All measurements are read-only.

- Commits: GitHub's compare (`gh api --cache 24h repos/WebKit/WebKit/compare/dbdca7545d...oven-sh:WebKit:0c06faad`, paginated) gives 2,245 commits ahead and 0 behind, 340 of them merges (337 with two parents, 3 with three). `git rev-list --count --merges dbdca7545d..0c06faad` in `~/jitcache` agrees on 340.
- Files and lines: `git diff --shortstat dbdca7545d 0c06faad -- Source/JavaScriptCore Source/WTF Source/bmalloc` gives 1,295 files, +52,801 / −8,288. Files with a changed line that is neither an `#include` nor blank, from `git diff -U0`: 647. So 648 files (half) change only includes. By lines, though, includes are 5,086 of 61,119 changed lines (8%), so "more than half of the volume" holds for neither files nor lines at this pin.
- Volume by directory (changed lines, files): `runtime/` 21,337 in 496; `ffi/` 8,445 in 29; `yarr/` 6,419 in 18; `bytecode/` 5,358 in 98; WTF 4,259 in 71; `heap/` 3,127 in 90; `dfg/` 1,601 in 34; `jit/` 830 in 55.
- `USE(BUN_JSC_ADDITIONS)` sites, with the doc's method (`#if`/`#elif` lines testing the macro in `.cpp`, `.h`, `.c` and `.mm` of JSC, WTF and bmalloc) in the pin worktree `~/collo-local/build/jitcache/webkit-pin` (at `0c06faad`, no JITCache commits): 624 in 186 files. By directory: `runtime/` 420, `heap/` 34, WTF 33, `ffi/` 30, `dfg/` 24, `bytecode/` 18, `tools/` 16, `jsc.cpp` 14, `parser/` 9, `ftl/` 8, `interpreter/` 7, `inspector/` 4, `bytecompiler/` 3, `wasm/` 2, `jit/` 1, `debugger/` 1, bmalloc 0.
- Baseline emitters: the fork now changes `JITCall.cpp` (call sites through `LazyCallLinkInfo`/`CallSiteData`, the direct-eval `operationEnsureCallLinkInfo`), `JITOpcodes.cpp` (`emit_op_new_reg_exp_shared`) and `JITPropertyAccess.cpp` (`ModuleVar` import slot, `LazyClosureVar`, `enumerator_put_by_val`'s structure test now including `Structure::s_hasImmutablePropertiesBits`), plus `CallLinkInfo::emitFastPathImpl`'s tail-call path and the LLInt (`LowLevelInterpreter64.asm`, `LLIntSlowPaths.cpp`). `jit/` still has one guarded site, and `BaselineJITCode.h` still adds the one field (`m_ownerWentAwayAt`). `iterator_close_check` and the inline fast-array path of `iterator_next` came from upstream.

## Proposed text

PS:

> PS: of the 2,245 commits that separate `WebKit/WebKit@dbdca7545d` from `oven-sh/WebKit@0c06faad`, 340 are merges, and half of the 1,295 files they change in JSC, WTF and bmalloc change only `#include` lines. What is left lives mostly in `runtime/`, `ffi/`, `yarr/`, `bytecode/` (the bytecode cache and the lazy linking) and `heap/`. The baseline JIT has a single guarded site and a new field in `BaselineJITCode`, but its call and scope templates changed with the lazy call data and module bindings, and `new_reg_exp_shared` is new.

Details table:

| metric | value |
|---|---|
| files changed in JSC + WTF + bmalloc | 1,295 (+52.8k / −8.3k lines) |
| files with a change other than `#include` | 647 |
| `USE(BUN_JSC_ADDITIONS)` sites | 624, in 186 files (same method) |
| per directory | `runtime/` 420 · `heap/` 34 · WTF 33 · `ffi/` 30 · `dfg/` 24 · `bytecode/` 18 · `parser/` 9 · `ftl/` 8 · `interpreter/` 7 · `bytecompiler/` 3 · `jit/` 1 · others 37 (`jsc.cpp` 14, `tools/` 16, `inspector/` 4, `wasm/` 2, `debugger/` 1) |

The "files with a change other than `#include`" method above also ignores blank lines; the old count's exact method is not recorded, so the two numbers may not be strictly comparable.
