# JITCache macro benches

These benches measure a serverless cold start with real libraries. A fresh Bun process sets a case up and then runs passes, each of which runs every unit of the case once, in order. With JITCache off, a process interprets, compiles and profiles its code during its first passes. A JITCache consumer installs the baseline code a producer committed, so its early passes should come closer to the producer's warmed ones. The design is in [docs/JitCache/drafts/macro-benches.md](../../docs/JitCache/drafts/macro-benches.md), and HARNESS.md's Benches section defines the numbers.

## Cases

| case | one pass | libraries |
|---|---|---|
| `api` | 100 requests with `fetch` to a server in the same process, one at a time: 40 `GET /users/:id`, 30 `POST /orders` whose every fifth body fails validation and gets a 400 with Zod's issues, 20 `GET /search?q=&sort=` that filter and sort 1,000 users, and 10 `GET /health`, in one seeded shuffled order | Hono 4.13 with Zod 4.6, served by `Bun.serve` on 127.0.0.1 with port 0 |
| `ssr` | renders the page of each of 10 shop categories with `renderToString`: a layout, a header, the navigation, breadcrumbs and a grid of 60 product cards with badges, rating stars, sale prices, color swatches and stock notes | React and React DOM 19.3 |
| `markdown` | renders 20 of Bun's documentation pages and wraps each in an HTML layout with a sidebar that lists all 20 | marked 18.1 |
| `tsc` | runs `ts.transpileModule` on 20 of Bun's TypeScript modules, with target ES2022, module ESNext and syntax diagnostics on | TypeScript 6.0.3 |

Each pass returns everything it produced: each response's status and body for `api`, and the HTML or the transpiled text for the others. `run.ts` hashes that output with SHA-256 after the pass's clock stops. Headers stay out of the hash, since Hono's timing middleware puts each request's duration in `Server-Timing`. The `api` server numbers orders in sequence, so its checksum changes from one pass to the next; the other cases produce the same output in every pass. In every case, pass k must give the same checksum in every process.

Setup builds every dataset with a seeded generator and reads every input file, so no pass touches the file system. A case checks its own output as it goes: `api` requires the status each request is due and the Server-Timing header on every response, `ssr` requires each page to end its document, and `tsc` fails on any diagnostic.

TypeScript stays at 6.0.3, the last release written in JavaScript. From 7.0 on, the `typescript` package ships the native compiler, an executable without `transpileModule`.

## Running a case by hand

```sh
cd PerformanceTests/JITCache
bun install
NODE_ENV=production bun run.ts api --passes=5
```

`run.ts` takes the case name and `--passes=N`, 34 by default. It refuses to run without `NODE_ENV=production`, the setting of a deployment: React picks its production or development build from it when it loads, and Bun picks the production or development JSX transform when the process starts, so the script cannot set it for itself.

The process writes JSON lines to standard output:

```
{"event":"ready","startupNanoseconds":118283896}
{"event":"pass","pass":1,"milliseconds":143.15,"checksum":"0ff02300..."}
...
{"event":"end","rssBytes":63188992}
```

`startupNanoseconds` is `Bun.nanoseconds()` once setup is done: imports loaded, inputs read, data built and, for `api`, the server listening. `milliseconds` is the wall time of one pass from `performance.now()`. `rssBytes` is `process.memoryUsage().rss` after the last pass. The process exits 0 after the end line, 1 when a case's check or a library fails, with the error on standard error, and 2 on a bad command line or without `NODE_ENV=production`.

JITCache's flags go before the script. A producer needs an artifact path that holds no artifact yet, and Bun commits the warmed bodies when the process exits:

```sh
NODE_ENV=production <bun> --jitcache=/tmp/macro-api --jitcache-mode=p --jitcache-bench-report=/tmp/producer.jsonl run.ts api
NODE_ENV=production <bun> --jitcache=/tmp/macro-api --jitcache-bench-report=/tmp/consumer.jsonl run.ts api
```

## The driver

`Tools/Scripts/jitcache-macro-bench.ts` runs the cases with a Bun that has JITCache, normally the release build from `bun build.ts bun-release`:

```sh
bun Tools/Scripts/jitcache-macro-bench.ts --bun=<Bun executable> [--cases=api,ssr,markdown,tsc] [--processes=M] [--passes=P] [--out=<directory>] [--timeout=<seconds>]
```

M is 5 and P is 34 by default. For each case the driver first runs one producer with `--jitcache=<fresh artifact> --jitcache-mode=p`, then M processes with JITCache off alternating with M consumers of the producer's artifact, so drift on the machine spreads over both. It runs one process at a time, in this directory, with `NODE_ENV=production`, `BUN_DEBUG_QUIET_LOGS=1` (a debug Bun's scoped logs would otherwise mix with the events) and no `JSC_` or `BUN_JSC_` variable. Every JITCache process writes a bench report. The driver holds the build lock, `${XDG_CACHE_HOME:-$HOME/.cache}/jitcache/build.lock`, while it measures, and says so when it has to wait for a build. A process that runs longer than `--timeout`, 1800 seconds by default, is killed.

A case fails when a process exits with anything but 0, lacks one of its events, or gives a pass a checksum that differs from another process's. A JITCache process also fails when its report has no `start` event that started its role, when Bun warns on standard error that JITCache turned itself off or stopped writing, when the producer committed no body, or when a consumer installed none. A failed case keeps its directory under the output directory, with every process's output, the bench reports and the artifact; a passed case's directory is deleted.

The output directory, by default `jitcache-macro/<time>` next to the Bun executable, receives two files. `results.jsonl` holds one record for the run, one per process (command, exit, wall time, startup, every pass's time and checksum, peak memory and the bench report's counts) and one per case. `summary.md`, which the driver also prints, gives for each case:

- startup and the first pass of the off processes (N1) and the consumers (C1), and C1/N1;
- W, the median of the producer's last five passes, and the curves Ck/W and Nk/W for k from 2 to P, with the first pass within 1.1 of W, a candidate for HARNESS.md's `catchUpMargin`;
- each mode's process wall time and peak resident set;
- the bench report's counts: installs, requests by action, lookups by result (an absent key is a miss), native baseline compiles, captures by trigger and outcome, committed bodies and bytes, and JITCache's CPU time in each.

The producer's own startup and first pass sit beside the others, since it records while it runs. Values over several processes are the minimum, with the median beside it. The driver exits 0 when every case passed, 1 otherwise, and 2 on a bad command line.

## Inputs

The inputs come from Bun's repository ([Davi-ldc/bun](https://github.com/Davi-ldc/bun), Bun 1.4.2, commit `f00137adb03e8fb602c5ebb5dd26a90226fef6f9`), copied unchanged; Bun is MIT-licensed. `inputs/markdown/<path>` is `docs/<path>` there and `inputs/ts/<path>` is `src/js/<path>`. Each set is the first 20 files by path, in byte order, within a size range: Markdown and MDX pages of 6 to 40 KiB, and TypeScript modules of 8 to 40 KiB without declaration files. From a Bun checkout, these commands list them:

```sh
cd docs && LC_ALL=C find . -type f \( -name '*.md' -o -name '*.mdx' \) -size +6143c -size -40961c -printf '%P\n' | LC_ALL=C sort | head -20
cd src/js && LC_ALL=C find . -type f -name '*.ts' ! -name '*.d.ts' -size +8191c -size -40961c -printf '%P\n' | LC_ALL=C sort | head -20
```

`package.json` pins each library's exact version, and `bun.lock` pins the rest.
