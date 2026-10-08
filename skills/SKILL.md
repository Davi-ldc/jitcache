---
name: jitcache
description: Serialize JavaScriptCore baseline code so a cold worker installs it instead of compiling
---

This is a hard problem and your instinct will be to route around it. But you are the world's strongest, largest language model — believe in yourself. We are after a timeless solution, one that somebody looking at it in a thousand years would still call optimal.

Pins:
- `bun-v1.4.2`, `744846f844374847c902b5e7fd59b4342a51ef99`

- **Webkitbun:** `oven-sh/WebKit` `2e2aa2290fac856d6f451ceacb58f7f5b44dd057`, declared by `WEBKIT_VERSION` in `scripts/build/deps/webkit.ts`

- **Upstream pin:** `WebKit/WebKit` at [`0d58b764f34b86ecf520ac7954b7aa1ded15cc5e`] through merge [`d4e7e206dc9b1c58eb8aca8f06eed7a6f4abb98a`]

PS: ARM = ARM64; we do not support arm32 or Windows ARM.

For cpp work, check [reference/cpp/cpp-coding-standards.md](reference/cpp/cpp-coding-standards.md). Run `clang-format` and `Tools/Scripts/check-webkit-style`, run from inside the checkout because its paths only resolve from there — it carries far more rules than the doc does, and its categories are spelled out in `Tools/Scripts/webkitpy/style/checkers/cpp.py`. It fires on the engine's own sources too, so read a hit as a question to answer, not a verdict to obey.
---

## Scope

[THREAD](../docs/JitCache/THREAD.md) decides what JITCache does now and what later versions add. DFG and FTL capture belong to later versions, so reviews do not report that a body which tiered up keeps its last baseline capture. Native JSC and Bun behavior is out of scope too, bugs included: whatever also happens with JITCache off, whether a deliberate choice or a defect, JITCache does not fix it, report it upstream or track it in todo.md, as long as it leaves the run no worse than native. Native behavior becomes a blocker only when it makes a consumer run differ from the JITCache-off run or makes a guarantee THREAD states false, and then the fix goes only as far as that guarantee needs.

## Writing SPECs

A SPEC is for the implementer, who builds from it alone. It states every decision, and gives a one-sentence reason only where the implementer would otherwise get the decision wrong. A SPEC's history is for whoever later revisits a decision: for each one that isn't obvious, it records what first seemed right, the evidence that changed it and the intent that came out, so the next agent doesn't go back to the obvious version. The SPEC links its history in its header and beside each decision the history explains, for whoever revisits that decision, but implementing never needs a link: a SPEC passage that only makes sense with its history is a defect. Logs and routine fixes belong in neither; git keeps them. Write so the reasoning reads in order: each rule lives in one place and is cited everywhere else, and each sentence states one fact plainly. Leave out provenance, status lines and anything a table column already says. [options.md](../docs/JitCache/options.md)'s opening is the model.

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

Unless a document explicitly says "in upstream WebKit", it refers to webkitbun, Bun's WebKit.

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

Do not search existing reports for duplicates. Use a descriptive, unique filename and add it without modifying existing reports or the documentation itself. Task-local implementation findings belong in the assigned SPEC.

The docs and the reports describe mechanisms, which outlive any single pin. Point to code by symbol (function, class, field, option), which a grep finds. Never cite line numbers. Cite a file path only when it is stable across WebKit versions and the symbol alone would not locate the code. Each doc is limited to 40,000 bytes, measured with `wc -c`; make room by condensing before adding.

Every edit to these docs requires human review before it is accepted.

Out of sequence: [bytecodeoptcodes.md](./reference/bytecodeoptcodes.md) — every bytecode opcode, grouped by family. Read if necessary. 

## Organization

This skill belongs to the JITCache repository, `Davi-ldc/jitcache` at `~/jitcache`, based on the `oven-sh/WebKit` fork. The Bun repository, `Davi-ldc/bun`, lives at `~/bun`, and its `vendor/WebKit` is a symlink to `~/jitcache`. Keep JITCache as self-contained as possible: concentrate new logic in its module and limit changes elsewhere in the engine to the necessary integration points.

Commands, environment, profiles and validation: the JITCache section of [CLAUDE.md](../CLAUDE.md).
