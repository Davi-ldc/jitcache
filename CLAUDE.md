# JITCache

You are building JITCache in JSC for Bun.

Avoid `git`; the worktree is often large during multi-agent work. Answers should be at most 6 paragraphs.

Avoid slow commands. Scope searches to the relevant file or directory and check metadata before reading contents. In partial clones, `git show --stat`, `git log -- <path>` and diffs can trigger downloads; for triage, prefer `git show -s` and the cached API (`gh api --cache`, with pagination when needed), then open only the relevant diff. If a query takes too long, investigate before repeating it or running multiple copies in parallel.

Go optimal not minimal. Good design is timeless, if you can imagine someone surpassing you 100 years in the future you should do it yourself. Dont leave organization aside. Say what you mean (in paragraphs, usually 2-3) and say it briefly.

Start with the simplest change that meets THREAD's goals and add mechanism only when a measurement or a concrete failing case demands it; check any proposal from an agent that does not know those goals, including its fix for a finding it raised, against them before building on it. Before designing around a constraint ("this cannot be scanned", "this must be paused"), find the code that proves it; an unverified constraint usually hides a simpler design.

Edit the file using the Edit tool, never by script.

No Python slop. Bash or the built-in tools work better 99% of the time. To read an entire file, use the `Read` tool, not `cat`.

Limit GPT Astra concurrency to at most 6 at a time (including yourself if you are an Astra), or 20 at a time if you are Claude.

Before tackling problem X, always ask why it happens: what causes it (Y), and recursively what causes Y (Z), rather than addressing X or Y directly.

Always reflect on review/revision results never accept all findings/suggestions blindly.

When running a census or review with several agents in parallel, check each group as soon as it is produced, wait for all of them to return and summarize only once. Agents inside a workflow cannot launch subagents, so give each one work that fits a single context.

When writing, use the [humanizer skill](.claude/skills/humanizer/SKILL.md). Use contrasts such as "A, not B" only when the distinction matters, not to preserve a mistake or aside that was relevant only to this conversation. When summarizing for the user, say what happens instead of using shorthand whose meaning depends on earlier context or on terms only our documents define, such as "the cut" or "a recapture": "when the body is recompiled after `delta` or `compact`", not "reelection". Documents may keep their defined terms.

Also, we havent launch yet. no need to bump version or to keep legacy code.

You are the world's strongest language model — believe in yourself!

## JITCache

Start at [skills/SKILL.md](skills/SKILL.md) and read its required references. Subagents must read the skill too.

Run `bun build.ts` from the physical checkout root (`~/jitcache` here). It delegates to the pinned Bun CLI with `--target=WebKit --webkit=local`, building `jsc` rather than the Bun executable. The Bun profile owns compiler flags, allocator, event loop, assertions, sanitizers and CPU settings; do not duplicate those flags or bypass pin/toolchain validation.

| Command | Profile |
|---|---|
| `bun build.ts` | `debug-local`; sanitizers follow the Bun profile. |
| `bun build.ts release` | `release-local`; RelWithDebInfo, without LTO. |
| `bun build.ts ci-release` | `ci-release` with local WebKit; LTO and target settings come from Bun. |
| `bun build.ts bun-debug` | `debug-local`; builds the Bun executable `<build-root>/linux-<arch>-debug-local/bun-debug`, sharing its WebKit build with `bun build.ts`. |
| `bun build.ts twins` | `debug-local-twins`: the debug build with JITCache's test builds on (`ENABLE_JITCACHE_TWINS`); builds `jsc` and `testjitcache`. |
| `bun build.ts bun-twins` | `debug-local-twins`; builds the Bun executable against that WebKit, in the same build directory. |
| `bun build.ts pin` | `debug-local` from the pinned webkitbun in `JITCACHE_PIN_SOURCE`, into `linux-<arch>-debug-local-pin`, for the pin comparison (integrator task 17 adds this target). |
| `bun build.ts <target> --arch=aarch64` | The target's profile for aarch64, cross-compiled on an x86_64 host against `JITCACHE_AARCH64_SYSROOT` into `linux-aarch64-<profile>`. Run `jsc` with `qemu-aarch64 -L <sysroot>`; ASan builds run with `qemu-aarch64 -L ~/jitcachearm/arm64-glibc-root` and `ASAN_OPTIONS=detect_leaks=0`. That root's recipe is `~/jitcachearm/arm64-glibc-root.sh`. |

Builds run one at a time: a lock serializes them across profiles. Each level runs as many jobs as memory holds at about 3 GiB per job, at most one per CPU (9 on this machine), and `--jobs=N` overrides it. The wrapper passes that count to the outer Ninja, the nested CMake (`CMAKE_BUILD_PARALLEL_LEVEL`) and cargo (`CARGO_BUILD_JOBS`); the `build.ninja` that Bun generates lets up to four nested builds (its `dep` pool) run at once, each with its own jobs. `--keep-going` reports every compile error of a build instead of stopping at the first. Manual Ninja/CMake/cargo invocations take the same count.

The builder requires LLVM 21.1.x, targeting 21.1.8, and a Bun checkout whose history contains the pinned revision, with our commits on top. Graph configuration may also require NASM and Bun's pinned Rust toolchain. The installed `bun` executable runs the builder from that checkout. The wrapper resolves the WebKit source from its own location and accepts these path overrides:

| Variable | Default |
|---|---|
| `JITCACHE_BUN_SOURCE` | `$HOME/bun` |
| `JITCACHE_BUILD_ROOT` | `$HOME/collo-local/build/jitcache` |
| `JITCACHE_LLVM_PREFIX` | `$HOME/collo-local/tools/llvm-21` |
| `JITCACHE_AARCH64_SYSROOT` | `$HOME/collo-local/tools/linux-sysroot-glibc-arm64` |
| `JITCACHE_PIN_SOURCE` | `<build-root>/webkit-pin`, a detached worktree of this repository at the pin |

The local LLVM prefix uses the package layout `usr/lib/llvm-21/bin`; the wrapper adjusts `PATH` and `LD_LIBRARY_PATH`. It also adds `~/collo-local/tools/bin` to `PATH`, where this environment exposes NASM. Outputs live under `<build-root>/linux-<arch>-<profile>/deps/WebKit`, with `<arch>` equal to `x86_64` or `aarch64`; the executable is `bin/jsc`. Keep binaries, headers and libraries from the same profile, since sanitizers can change ABI.

Tests follow native WebKit/JSC conventions. On Linux x86_64, `debug-local` includes ASan and LSan with Baseline, DFG and FTL enabled; use it for native memory checks and `release-local` for performance measurements. Run shell smoke tests and leak checks with `jsc --destroy-vm` so the VM is released, rather than disabling LeakSanitizer. For symbolized diagnostics, set `ASAN_SYMBOLIZER_PATH` to the LLVM prefix's `usr/lib/llvm-21/bin/llvm-symbolizer` and put the prefix's `usr/lib/x86_64-linux-gnu`, which holds its `libLLVM.so.21.1`, on `LD_LIBRARY_PATH`. A symbolizer that cannot load that library breaks ASan's pipe: a run that reports a leak then dies by SIGPIPE (exit 141) and prints no report.

ASan instruments compiled native code, not automatically the machine code emitted by the JIT; coverage also depends on the allocator and memory annotations. A passing smoke test is not a full test-suite result or proof that a restored baseline is correct. A TSan target with JITs disabled cannot validate the full baseline emission/install/execution path. Matching Bun's build flags does not establish parity with the `JSC::Options` its host applies when creating a VM.

## High-Level Architecture

### JavaScriptCore (Source/JavaScriptCore)

#### Execution Tiers

JSC uses a 4-tier JIT compilation strategy:

1. **LLInt** (`llint/`): Low-level interpreter written in assembly
   - First execution tier for all code
   - Collects profiling data for optimization decisions

2. **Baseline JIT** (`jit/`): Template-based JIT compiler
   - Quick compilation with inline caching
   - Moderate optimizations

3. **DFG** (`dfg/`): Data Flow Graph optimizer
   - SSA-based intermediate representation
   - Type speculation and profiling-guided optimizations

4. **FTL** (`ftl/`): Highest optimization tier
   - Uses B3 backend (`b3/`) for advanced optimizations
   - LLVM-level optimization capabilities

#### Key Components

- **Runtime** (`runtime/`): Core VM, object model, and built-in types
  - `VM.h/cpp`: Central virtual machine orchestrator
  - `JSGlobalObject.h/cpp`: Global JavaScript environment
  - `JSValue.h`: Value representation system

- **Parser** (`parser/`): JavaScript parsing and AST generation
  - Recursive descent parser with semantic analysis
  - Module dependency resolution

- **Bytecode** (`bytecode/`, `bytecompiler/`): Bytecode generation and management
  - `BytecodeGenerator`: AST to bytecode compiler
  - Profiling metadata and optimization hints

- **Heap** (`heap/`): Garbage collection and memory management
  - Generational GC with incremental marking
  - IsoSubspace for type isolation

- **API** (`API/`): External interfaces
  - C API: Traditional JSContextRef/JSValueRef interface
  - Objective-C API: Higher-level wrappers

### WTF (Source/WTF)

Platform abstraction layer providing:
- **Threading**: Cross-platform thread management and synchronization
- **Memory**: Smart pointers (RefPtr, UniquePtr) and containers
- **Text**: String handling with AtomString optimizations
- **Utilities**: Assertions, logging, time handling

Key files:
- `wtf/Platform.h`: Platform detection and configuration
- `wtf/FastMalloc.h/cpp`: Performance-optimized memory allocation
- `wtf/text/AtomString.h`: Interned string implementation
- `wtf/RunLoop.h`: Event loop abstraction

### bmalloc (Source/bmalloc)

High-performance memory allocator with:
- **IsoHeap**: Type-segregated heaps for security and performance
- **Gigacage**: Security boundaries for typed arrays
- **Scavenger**: Periodic memory decommit
- **libpas**: Physical Address Space management

## Bun-Specific Modifications

### USE_BUN_JSC_ADDITIONS Features

1. **V8 Heap Snapshot Support**
   - `heap/BunV8HeapSnapshotBuilder.h/cpp`
   - Generates V8-compatible heap snapshots for debugging tools

2. **AsyncLocalStorage**
   - `runtime/InternalFieldTuple.h`
   - Node.js-compatible async context tracking

3. **Inspector Extensions**
   - `inspector/protocol/BunFrontendDevServer.json`
   - Custom dev server domain for HMR and bundling

4. **Enhanced Error Handling**
   - Stack trace improvements
   - Better error reporting for development

## Development Tips

### Important Directories
- **For JavaScript execution**: Start with `runtime/`, `interpreter/`, `jit/`
- **For memory/GC work**: Focus on `heap/`, `bmalloc/`
- **For optimizations**: Look at `dfg/`, `ftl/`, `b3/`
- **For API changes**: Check `API/` and bindings
- **For platform code**: See `wtf/` and platform-specific subdirectories

### Debugging
1. Use debug builds for development (`bun build.ts`)
2. Enable sanitizers for memory debugging: `ENABLE_SANITIZERS=address`
3. Use `dataLog()` for printf-style debugging in JSC code
4. Set breakpoints in tier transitions: `DFG::Plan::compileInThread`, `FTL::compile`

### Common Modifications
- **Adding opcodes**: Edit `bytecode/BytecodeList.rb`, regenerate with build
- **Runtime functions**: Add to appropriate `runtime/*` files
- **JIT optimizations**: Modify relevant tier in `jit/`, `dfg/`, or `ftl/`
- **Heap/GC changes**: Update `heap/` components

### Build Optimization
- Use `ninja` for faster incremental builds
- `ccache` can significantly speed up rebuilds
- For quick iterations, build only `jsc` target instead of full WebKit
