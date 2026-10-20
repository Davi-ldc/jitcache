# JITCache benches

Bun 1.4.3 with JITCache, the aarch64 `bun-ci-release` build, on an Oracle Ampere A1 (Neoverse-N1, 4 OCPU, 24 GB), 2026-10-20. Each case is a small app in `apps/`, bundled with `bun build --target=bun` and called the way a serverless platform calls a function, with the same request every call. A producer served 100 calls and saved the JIT code with delta(vm) at exit, and a process with JITCache read it from a warm page cache (assuming you will be downloading it). Every Bun process ran pinned to one CPU. Times are the median of 150 processes without and with JITCache, from five runs of the bench. [allresults.md](allresults.md) has every table, a different request every call, the 95% intervals and the runs from a cold disk.

| case | requests 2-35 | requests 2-500 | startup + request 1 |
|---|---|---|---|
| hono | 48% faster | 15% faster | 24.5 → 15.7 ms |
| elysia | 41% faster | 4% faster | 53.3 → 28.1 ms |
| react-ssr | 21% faster | 4% faster | 46.9 → 29.5 ms |
| preact-ssr | 7% faster | 3% faster | 16.5 → 12.9 ms |
| webhook | 63% faster | 18% faster | 28.6 → 19.8 ms |

# Current state

![JSC JIT tiers with JITCache](../../docs/images/jitcache_stair.png)

# Difference

Requests 2-35 sum the 34 calls after the first in each 50-call process, and requests 2-500 every call after the first in each 500-call process. Times are in ms.

Requests 2-35:

| case | without (ms) | with (ms) | faster |
|---|---|---|---|
| hono | 3.69 | 1.91 | 48% |
| elysia | 11.5 | 6.78 | 41% |
| react-ssr | 63.3 | 50.3 | 21% |
| preact-ssr | 28.9 | 27.0 | 7% |
| webhook | 4.26 | 1.56 | 63% |

Requests 2-500:

| case | without (ms) | with (ms) | faster |
|---|---|---|---|
| hono | 19.9 | 16.9 | 15% |
| elysia | 76.0 | 72.9 | 4% |
| react-ssr | 399 | 384 | 4% |
| preact-ssr | 173 | 168 | 3% |
| webhook | 25.5 | 20.9 | 18% |

## Startup

Bun's startup plus the first request, in ms, and how many ms JITCache saved.

| case | without (ms) | with (ms) | saved (ms) |
|---|---|---|---|
| hono | 24.5 | 15.7 | 8.8 |
| elysia | 53.3 | 28.1 | 25.2 |
| react-ssr | 46.9 | 29.5 | 17.4 |
| preact-ssr | 16.5 | 12.9 | 3.6 |
| webhook | 28.6 | 19.8 | 8.8 |


To run it: `bun bench.ts --bun=<a bun-ci-release build> --variants=bundle` in this directory, after `bun install --frozen-lockfile`.
