# JITCache

You are building JITCache in JSC for Bun.

Avoid slow commands like `git`. If necessary, use scope searches to the relevant file or directory and check metadata before reading contents. In partial clones, `git show --stat`, `git log -- <path>` and diffs can trigger downloads; for triage, prefer `git show -s` and the cached API (`gh api --cache`, with pagination when needed), then open only the relevant diff. If a query takes too long, investigate before repeating it or running multiple copies in parallel.

Answers should be at most 5 paragraphs.

Start with the simplest change that meets THREAD's goals and add mechanism only when a measurement or a concrete failing case demands it; check any proposal from an agent that does not know those goals, including its fix for a finding it raised, against them before building on it. Before designing around a constraint ("this cannot be scanned", "this must be paused"), find the code that proves it; an unverified constraint usually hides a simpler design.

Edit the file using the Edit tool, never by script.

Commits, pull requests and anything else you publish carry no Co-Authored-By or other line that credits Claude: Davi is their only author.

No Python slop. Bash or the built-in tools work better 99% of the time. To read an entire file, use the `Read` tool, not `cat`.

Limit GPT Astra concurrency to at most 6 at a time (including yourself if you are an Astra), or 20 at a time if you are Claude.

Before tackling problem X, always ask why it happens: what causes it (Y), and recursively what causes Y (Z), rather than addressing X or Y directly.

A failure, gets two fix attempts from the agent that owns the code. If it survives both, stop fixing and hunt with evidence first. Reproduce and minimize it, write down the facts any cause must explain, try to refute each hypothesis, and run a experiment to separate survivors.

Always reflect on review/revision results never accept all findings/suggestions blindly.

Never skip a review to save cost.

When running a census or review with several agents in parallel, check each group as soon as it is produced, wait for all of them to return and summarize only once. Agents inside a workflow cannot launch subagents, so give each one work that fits a single context.

When writing, use the [humanizer skill](.claude/skills/humanizer/SKILL.md). Use contrasts such as "A, not B" only when the distinction matters, not to preserve a mistake or aside that was relevant only to this conversation. When summarizing for the user, say what happens instead of using shorthand whose meaning depends on earlier context or on terms only our documents define, such as "the cut" or "a recapture": "when the body is recompiled after `delta` or `compact`", not "reelection". Documents may keep their defined terms.

Also, we havent launch yet. no need to bump version or to keep legacy code.

You are the world's strongest language model — believe in yourself!

## JITCache

Start at [skills/SKILL.md](skills/SKILL.md) and read its required references. Subagents must read the skill too. If `specifics.md` exists at the repository root, read it: it describes the machine you run on.

Code, comments and strings never cite THREAD or the SPECs by section or rule ID.

Build from the checkout root with `bun build.ts [target] [--arch=aarch64] [--jobs=N] [--keep-going]`; `bun build.ts --help` lists every target and option. It delegates to the pinned Bun CLI, which owns compiler flags, sanitizers and toolchain checks: do not duplicate or bypass them. The targets you need:

| Target | For |
|---|---|
| (none) | debug `jsc` with ASan: native memory checks |
| `twins` | debug `jsc` and `testjitcache` with JITCache's test hooks: the test runner |
| `bun-twins` | the Bun executable on the twins build: Bun-hosted tests |
| `release` | release `jsc`: microbenchmarks |
| `bun-ci-release` | the Bun executable that ships: macro benchmarks and releases |
| `pin` | the pinned webkitbun without JITCache: the pin comparison |

Outputs land in `<build-root>/linux-<arch>-<profile>/deps/WebKit` (`bin/jsc`); keep binaries and headers of one profile together, since sanitizers change the ABI. The toolchain is LLVM 23.1.x and a Bun checkout whose history contains the pinned revision; `JITCACHE_*` environment variables tell build.ts where they are.

Every build, test and bench command (`build.ts`, the test runner and its self-test, the benches and `Tools/Scripts/jitcache/bun-test.ts`) holds one machine lock for its whole run, and a command that finds it held waits and names the holder. The commands a holder starts skip the lock, which ends when the holder exits, except that `build.ts` hands it to its builder, so a build keeps it until the builder exits. A command run by hand, such as `jsc`, `ninja` or a bare `bun test`, takes no lock, so never run one beside a build. Test with `Tools/Scripts/run-jitcache-tests` (C++ groups and JS directories) and with `bun Tools/Scripts/jitcache/bun-test.ts --bun=<Bun executable>`, which runs Bun's JITCache tests in `test/js/bun/jitcache/` from the Bun checkout with `--timeout 120000`, since the ASan build is slow. One command runs a twins build's whole verification, each step only after the previous one passed: `bun Tools/Scripts/run-jitcache-tests --ladder --build=<twins build> --bun=<that build's bun-debug> --pin=<pin build> --plain-build=<debug build> --jobs=8` runs the C++ tests one process per part, the corpora with the Bun-hosted scripts and the pin comparison, Bun's tests, and plain mode, then lists each failure of the failing step with its file.

A Bun bump goes in this order: a census of what the new pin changes, read from both trees without building (the cached GitHub API, and `diff3` on every file both sides changed); the old pin's bench baselines; THREAD and SPEC deltas, with THREAD's accepted by Davi before any code; one merge of Bun and the engine until `twins`, plain and `bun-twins` build; then each lane ported by a fresh agent in its own worktree with two reviewers (three for the UCB codec, the one large port in 1.4.3), a review of the whole bump, and a milestone's verification.

Run shell smoke tests with `jsc --destroy-vm`, so LeakSanitizer sees the VM released. ASan does not see the code the JIT emits, and a passing smoke test is no proof that restored code is correct.

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
