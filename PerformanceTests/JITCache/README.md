# JITCache macro benches

Small serverless apps in Bun, timed from process start to exit with JITCache off and with JITCache on.

## How we measure

Each case in `apps/` is a small app with a fetch handler: `hono`, `elysia`, `react-ssr`, `preact-ssr` and `webhook` (`pdf` runs only when named). A call sends one request to that handler, the way a serverless platform invokes a function. Each case has 10 request variants in `requests/`, and runs in two branches: fixed, where every call sends variant 1, and varied, where the calls cycle through all 10.

Every process runs Bun as shipped, with concurrent JIT on, pinned to one CPU with `taskset`.

1. A producer serves 100 calls and saves what it learned when it exits (`--jitcache-delta-at-exit`).
2. Consumers of that artifact run 50 calls or 500 calls, beside JITCache-off processes of the same Bun build.
3. With 500 calls, consumers of a producer that never saves at exit run too; that artifact holds only what was captured at each baseline compile.
4. Cold consumers start with the artifact's files dropped from the page cache.

Each comparison runs 3 times, each time with fresh producers and 10 processes per mode, interleaved so drift spreads across modes. We report the time to finish, the CPU time of all threads, and the time after 1, 10, 50, 100 and 500 calls. Timed processes write no JITCache report; separate untimed runs write it for per-body numbers, and a census run counts DFG compiles per call. The goal is a real case at least 50% faster to finish with JITCache.

The headline number is per-request latency, consumers against JITCache off: the median and p90 of request k for k = 1 to 50, of the sum of requests 2 to 35 in each process, and of startup plus request 1, where startup is Bun's clock when `run.ts` is ready to serve. `summary.md` gives requests 1, 2, 3, 5, 10, 20, 35 and 50, the sum and startup plus request 1, and `request-curves.tsv` beside it holds every request up to 50. Time to finish comes second, with the exit apart: `run.ts` prints Bun's clock on its last line, and the exit time is the wall time left after it.

The census counts baseline compiles per call too, beside the DFG and FTL ones; a baseline compile counts when the JIT worker starts it, since JSC logs no baseline request. Cold processes come in pairs: a consumer and a JITCache-off process both start after Bun's binary, this directory with its `node_modules` and the run's own transpiler cache leave the page cache, and the consumer's artifact as well. The driver checks with `mincore` that every page left and records what each process read from storage. Two comparisons are optional: `--capture=all,compiled` runs producers under both of Bun's `--jitcache-capture` scopes, and `--compare-llint-progress` adds consumers whose imported bodies arm their LLInt counters from zero.

## Running

On x86_64, from the repository root:

```sh
bun build.ts bun-ci-release
cd PerformanceTests/JITCache && bun install --frozen-lockfile && cd ../..
bun Tools/Scripts/jitcache-macro-bench.ts --bun=$HOME/collo-local/build/jitcache/linux-x86_64-ci-release/bun
```

On ARM64, build on x86_64 with `bun build.ts bun-ci-release --arch=aarch64`, copy `PerformanceTests/JITCache/` (without `node_modules`), `Tools/Scripts/jitcache-macro-bench.ts` and the built `bun` to the ARM64 machine, keeping the first two at the same paths under one directory, install a stock Bun there to run the driver, and run the same two commands. The machine needs `taskset`, `dd`, `flock` and `who`.

Results go to `jitcache-macro/<time>/` next to the Bun binary, or to `--out=<directory>`: `summary.md`, `results.jsonl` and the artifacts. `--check` makes a short correctness run, and the driver lists every flag when it gets one it cannot read. One case runs by hand from this directory with `NODE_ENV=production bun run.ts <case> --calls=N --branch=fixed|varied`.

## Results

Ampere A1 (2 Neoverse-N1 vCPUs), aarch64 release Bun, every process pinned to one CPU. Time to finish, median of 30 processes; below 1.00 means JITCache is faster.

| case | branch | calls | JITCache off (ms) | consumer (ms) | consumer / off |
|---|---|---|---|---|---|
| | | | | | |


The PDF inputs are public domain; their sources are in `inputs/pdf/SOURCES.md`.
