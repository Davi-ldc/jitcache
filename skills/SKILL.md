---
name: jitcache
description: Serialize JavaScriptCore baseline code so a cold worker installs it instead of compiling
---

This is a hard problem and your instinct will be to route around it. But you are the world's strongest, largest language model — believe in yourself. We are after a timeless solution, one that somebody looking at it in a thousand years would still call optimal.

Pins:
- `bun-v1.4.3`, `c6da4a4d3010e5553438c60f6bd76d981976867c`

- **Webkitbun:** `oven-sh/WebKit` `0c06faadf65bf8e8c8ad3a5a8aca83e1e9ed653f`, declared by `WEBKIT_VERSION` in `scripts/build/deps/webkit.ts`

- **Upstream pin:** `WebKit/WebKit` at [`dbdca7545da84620b857f7d32749a1ee364370f0`] through merge [`661e26ef1300c45debd5f73dd65ecd4cba1daad8`]

PS: ARM = ARM64; we do not support arm32 or Windows ARM.

For cpp work, check [reference/cpp/cpp-coding-standards.md](reference/cpp/cpp-coding-standards.md). Run `clang-format` and `Tools/Scripts/check-webkit-style`, run from inside the checkout because its paths only resolve from there — it carries far more rules than the doc does, and its categories are spelled out in `Tools/Scripts/webkitpy/style/checkers/cpp.py`. Format JITCache's own files whole and an engine file only on the lines you changed (`clang-format --lines`), since formatting a whole engine file rewrites upstream code. It fires on the engine's own sources too, so read a hit as a question to answer, not a verdict to obey. JITCache's own files carry no header at all, no license and no copyright, only comments that explain code, so its `legal/copyright` hit does not apply to them.
---

## Scope

[THREAD](../docs/JitCache/THREAD.md) decides what JITCache does now and what later versions add. DFG and FTL capture belong to later versions, so reviews do not report that a body which tiered up keeps its last baseline capture. Native JSC and Bun behavior is out of scope, bugs included, except as THREAD's Native fixes allows: a native defect THREAD does not name is not fixed, reported or tracked in todo.md. Never propose upstreaming anything.

## Decisions

Product is what JITCache promises or exposes: THREAD's guarantees, the public surface (the C++ API, Bun's flags and their defaults, what `status`, `delta` and maintenance report), the artifact's compatibility and trust model, the supported platforms, v1's scope and anything a Bun user or embedder can observe. How the code meets those is not product: layouts, internal interfaces, the task split and order, codec internals, tests, benches, builds and the workflow. A product question waits for Davi, with your recommendation, and a question you cannot place is product. Decide everything else yourself, for THREAD's goals, and write each decision where its rule lives: in CLAUDE.md or this skill when it is a working rule; in todo.md when it leaves work open; and in the plan of the work under way, such as a bump plan, until that work lands. The SPECs in `docs/JitCache/specs/` record v1 as it shipped and are not updated until the DFG work replaces them: where a SPEC disagrees with THREAD or the code, THREAD and the code win.

## Writing SPECs

A SPEC is for the implementer, who builds from it alone. It states every decision, and gives a one-sentence reason only where the implementer would otherwise get the decision wrong. A SPEC's history is for whoever later revisits a decision: for each one that isn't obvious, it records what first seemed right, the evidence that changed it and the intent that came out, so the next agent doesn't go back to the obvious version. The SPEC links its history in its header and beside each decision the history explains, for whoever revisits that decision, but implementing never needs a link: a SPEC passage that only makes sense with its history is a defect. Logs and routine fixes belong in neither; git keeps them. Write so the reasoning reads in order: each rule lives in one place and is cited everywhere else, and each sentence states one fact plainly. Leave out provenance, status lines and anything a table column already says. [options.md](../docs/JitCache/options.md)'s opening is the model. The comments in a SPEC's code blocks are the comments the implementer writes, so they explain the code in their own words and never cite THREAD, a SPEC, a section, a rule ID, a task, a step or a phase; a citation belongs in the prose around the block.

## Glossary

**CB** = `CodeBlock`; **UCB** = `UnlinkedCodeBlock`; **IC** = inline cache; **ops** = operations; **obj** = object;
**Thunk** = shared code every tier calls into — the default call target, the slow-path cache handlers. Generated once per process when it embeds no `VM*` (`sharedCommonThunks()`), otherwise once per VM, eagerly or on first use.
**Jump island** = a four-byte `b` allocated in a reserved band, the only way an ARM64 jump reaches a target further than its own range; it holds no logic
**Watchpoint** = a subscriber list of functions attached to one fact, fired when it stops being true.
**OSR** = On-Stack Replacement, swapping the code of a call frame that is already on the stack, without waiting for the function to return.
**OSR exit** = leaving optimized code whose speculation failed, landing in the baseline/llint at the equivalent point.
**OSR entry** = climbing into optimized code mid-execution, needed for a function called once that then loops.
**WebKit** = upstream `WebKit/WebKit`.
**webkitbun** = `oven-sh/WebKit`, Bun's fork of WebKit.

## Internal docs

The documents describe the engine as it stands in the current state of the JITCache repository ([Davi-ldc/jitcache](https://github.com/Davi-ldc/jitcache)): webkitbun at the pin plus our commits, which change some engine behavior. Where the code and a document disagree, the code wins and the document needs a report. A document says "in upstream WebKit" or "at the pin" when it means another version.

These documents explain JSC's internal processes and structures so you can spend time on our logic rather than rediscovering theirs. They are product agnostic: they describe only the engine and never mention JITCache, while JITCache decides what they cover and how deep they go. Text a report proposes for them follows the same rule. Read them (full file) in the order below. The common files and basics are mandatory; the rest depend on the tier you are working on. The links below are the English versions, the only ones maintained. Agents and subagents ignore `reference/pt/`: do not read, cite or edit anything under it.

1. [basics.md](./reference/basics.md) 
2. [common/profiles.md](./reference/knowledge/common/profiles.md)
3. [common/inlinecaches.md](./reference/knowledge/common/inlinecaches.md)
4. [common/structures.md](./reference/knowledge/common/structures.md) — comes after the caches because watchpoints and the rare-data caches are explained against what an IC assumes.
5. [baseline/compiler.md](./reference/knowledge/baseline/compiler.md) — when a baseline compilation runs and on which thread, what it reads, the four stages that emit and the four that link, the artifact it hands to installation, and what the finished code depends on: every reference fixed in its bytes, by owner, and the per-CB facts it bakes when the CBs of one body share it.
6. [baseline/install.md](./reference/knowledge/baseline/install.md) — how a CodeBlock receives baseline code: the three doors, two of which install code that CodeBlock never compiled, plus the embedder route, and the GC and thread conditions around them; the four steps, from publishing the artifact to relinking the incoming calls; how execution enters the code and how the parked artifact reaches the body's other CodeBlocks; the cache molds it converts field by field; and what it assumes without ever checking.
7. [common/locks.md](./reference/knowledge/common/locks.md) — who takes each lock across JS execution, compilation and GC, and where the compiler safepoints release it.
8. [common/bunpatches.md](./reference/knowledge/common/bunpatches.md)

If you find a documentation error, or an addition important to JITCache as a whole rather than only to your current task, create a new Markdown file in repository-root `report/`. Use one file per point, briefly describing the affected documentation, proposed change and supporting reasoning or evidence. Each report names one documentation file; split a point that affects several files into one report per file. When a report you resolve also concerns another file, fix your part and rewrite the report so it names only that other file.

Do not search existing reports for duplicates. Name the report after the documentation file it names, without `.md`, then a hyphen and a short description of the point: `inlinecaches-<point>.md`. The two `compiler.md` files add their folder: `baseline-compiler-<point>.md` and `dfg-compiler-<point>.md`. Each document's maintainer finds its reports by that prefix. Add the report without modifying existing reports or the documentation itself. Task-local implementation findings belong in the assigned SPEC.

The docs and the reports describe mechanisms, which outlive any single pin. Point to code by symbol (function, class, field, option), which a grep finds. Never cite line numbers. Cite a file path only when it is stable across WebKit versions and the symbol alone would not locate the code. Each doc is limited to 40,000 bytes, measured with `wc -c`; make room by condensing before adding.

Every edit to these docs requires human review before it is accepted.

Out of sequence: [bytecodeoptcodes.md](./reference/bytecodeoptcodes.md) — every bytecode opcode, grouped by family. Read if necessary. 

## Organization

This skill belongs to the JITCache repository, `Davi-ldc/jitcache` at `~/jitcache`, based on the `oven-sh/WebKit` fork. The Bun repository, `Davi-ldc/bun`, lives at `~/bun`, and its `vendor/WebKit` is a symlink to `~/jitcache`. Keep JITCache as self-contained as possible: concentrate new logic in its module and limit changes elsewhere in the engine to the necessary integration points.

Commands, environment, profiles and validation: the JITCache section of [CLAUDE.md](../CLAUDE.md).
