# JITCache benches

The beta run on a Oracle Ampere A1 (Neoverse-N1, 4 OCPU, 24 GB) with the aarch64 shipping Bun, 2026-10-14. Each case is a small app in `apps/`, called the way a serverless platform calls a function, with the same request every call. A producer served 100 calls and saved with delta(vm) at exit, and the processes with JITCache read its cache from a warm page cache (assuming you will be dowloading it). Every Bun process ran pinned to one CPU.

# difference 

Requests 2-35 sum the 34 calls after the first in each 50-call process, and requests 2-500 every call after the first in each 500-call process. Times are in ms, the median of 30 processes without and with JITCache; "faster" is how much less time the process with JITCache took.

Requests 2-35:

| case | without (ms) | with (ms) | faster |
|---|---|---|---|
| hono | 1.37 | 1.07 | 22% |
| elysia | 10.2 | 7.51 | 27% |
| react-ssr | 54.0 | 45.8 | 15% |
| preact-ssr | 29.1 | 25.9 | 11% |
| webhook | 3.37 | 1.71 | 49% |

Requests 2-500:

| case | without (ms) | with (ms) | faster |
|---|---|---|---|
| hono | 18.4 | 16.3 | 12% |
| elysia | 77.1 | 74.1 | 4% |
| react-ssr | 451 | 444 | 1% |
| preact-ssr | 163 | 159 | 2% |
| webhook | 22.8 | 20.0 | 12% |

## cold start

Bun's startup plus the first request, in ms, the median of 30 processes, and how many ms JITCache saved.

| case | without (ms) | with (ms) | saved (ms) |
|---|---|---|---|
| hono | 53.4 | 48.3 | 5.1 |
| elysia | 107 | 90.8 | 16.2 |
| react-ssr | 49.6 | 30.8 | 18.8 |
| preact-ssr | 16.7 | 15.3 | 1.4 |
| webhook | 49.9 | 45.7 | 4.2 |


To run it: `bun bench.ts --bun=<a bun-ci-release build>` in this directory, after `bun install --frozen-lockfile`.
