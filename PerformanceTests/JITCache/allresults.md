# JITCache benches: all results

Bun 1.4.3 with JITCache (the aarch64 `bun-ci-release` build, revision 1.4.3-canary.1+238aa5f08) on an Oracle Ampere A1 (Neoverse-N1, 4 OCPU, 24 GB), 2026-10-20. Each case is a small app in `apps/`, bundled with `bun build --target=bun` and called the way a serverless platform calls a function: the fixed branch sends the same request every call, the varied branch a different one. A producer served 100 calls and saved its JIT code with `delta` at exit; a process with JITCache then read that artifact. Every Bun process ran pinned to one CPU.

The numbers pool five runs of `bun bench.ts --variants=bundle`, each with 3 repetitions of 10 processes per mode, so every warm cell is the median of 150 processes without JITCache and 150 with it; each repetition has its own producer. "95% interval" is a bootstrap interval of the pooled result, and "runs" gives the lowest and highest result of the five runs taken alone. Times are in ms.

## Warm page cache

Startup plus request 1, Bun's clock at the ready line plus the first call:

| case | branch | without | with | faster | 95% interval | runs |
|---|---|---|---|---|---|---|
| hono | fixed | 24.5 | 15.7 | 36% | 35 to 37% | 35 to 37% |
| hono | varied | 24.4 | 15.5 | 36% | 36 to 37% | 35 to 37% |
| elysia | fixed | 53.3 | 28.1 | 47% | 47 to 48% | 47% |
| elysia | varied | 53.1 | 29.0 | 46% | 45 to 46% | 45 to 46% |
| react-ssr | fixed | 46.9 | 29.5 | 37% | 37 to 38% | 37 to 38% |
| react-ssr | varied | 47.1 | 31.0 | 34% | 33 to 35% | 33 to 35% |
| preact-ssr | fixed | 16.5 | 12.9 | 22% | 21 to 23% | 21 to 23% |
| preact-ssr | varied | 16.5 | 13.3 | 19% | 18 to 21% | 18 to 21% |
| webhook | fixed | 28.6 | 19.8 | 31% | 30 to 32% | 30 to 32% |
| webhook | varied | 28.4 | 20.0 | 30% | 29 to 31% | 29 to 30% |

Request 3, the third call alone:

| case | branch | without | with | faster | 95% interval | runs |
|---|---|---|---|---|---|---|
| hono | fixed | 0.040 | 0.049 | 22% slower | 3 to 85% slower | 3 to 41% slower |
| hono | varied | 0.109 | 0.157 | 44% slower | 35 to 49% slower | 25 to 56% slower |
| elysia | fixed | 0.146 | 0.157 | 7% slower | 2 to 10% slower | 2 to 9% slower |
| elysia | varied | 2.78 | 4.46 | 61% slower | 58 to 63% slower | 60 to 62% slower |
| react-ssr | fixed | 3.22 | 0.769 | 76% | 75 to 77% | 75 to 77% |
| react-ssr | varied | 0.887 | 0.205 | 77% | 75 to 80% | 69 to 82% |
| preact-ssr | fixed | 0.904 | 2.75 | 204% slower | 23% faster to 215% slower | 30% faster to 208% slower |
| preact-ssr | varied | 0.145 | 0.699 | 382% slower | 356 to 398% slower | 27% faster to 746% slower |
| webhook | fixed | 0.051 | 0.057 | 12% slower | 10 to 14% slower | 6 to 22% slower |
| webhook | varied | 1.10 | 0.539 | 51% | 24 to 54% | 25 to 54% |

On one CPU, request 3 shows which early call the scheduler gives to a background compile. With JITCache, preact-ssr requests the DFG compile of its largest function, about 10 ms of CPU, in call 3 instead of call 8, since its imported bodies carry their counter progress. That compile runs on JSC's compiler thread, which shares the process's one CPU, and the main thread's own work in call 3 stays at 0.6 to 0.8 ms. In Bun 1.4.3 the compiler thread gets the CPU slightly sooner than in 1.4.2 and its first slice lands in call 3; in 1.4.2 it lands just after it, and a 250 µs pause at startup moves it again. The work is the same in both versions: compiled on the main thread instead, both pay about 10 ms in call 3 and the same total over calls 2 to 50. With two CPUs per process, request 3 is faster with JITCache (0.708 → 0.581 ms). The windows below include the compile wherever it lands.

Requests 2 to 35, the 34 calls after the first in each 50-call process:

| case | branch | without | with | faster | 95% interval | runs |
|---|---|---|---|---|---|---|
| hono | fixed | 3.69 | 1.91 | 48% | 46 to 54% | 46 to 55% |
| hono | varied | 9.99 | 7.63 | 24% | 22 to 25% | 21 to 28% |
| elysia | fixed | 11.5 | 6.78 | 41% | 41 to 42% | 40 to 42% |
| elysia | varied | 17.9 | 14.7 | 18% | 17 to 19% | 17 to 19% |
| react-ssr | fixed | 63.3 | 50.3 | 21% | 19 to 22% | 20 to 21% |
| react-ssr | varied | 25.5 | 17.7 | 31% | 29 to 33% | 28 to 32% |
| preact-ssr | fixed | 28.9 | 27.0 | 7% | 5 to 8% | 5 to 8% |
| preact-ssr | varied | 11.6 | 10.3 | 11% | 9 to 14% | 8 to 14% |
| webhook | fixed | 4.26 | 1.56 | 63% | 60 to 67% | 58 to 67% |
| webhook | varied | 10.7 | 7.06 | 34% | 33 to 35% | 28 to 37% |

Requests 2 to 500, every call after the first in each 500-call process:

| case | branch | without | with | faster | 95% interval | runs |
|---|---|---|---|---|---|---|
| hono | fixed | 19.9 | 16.9 | 15% | 13 to 16% | 13 to 16% |
| hono | varied | 44.2 | 39.7 | 10% | 9 to 12% | 8 to 13% |
| elysia | fixed | 76.0 | 72.9 | 4% | 4 to 5% | 4 to 5% |
| elysia | varied | 97.5 | 87.2 | 11% | 10 to 11% | 10 to 11% |
| react-ssr | fixed | 399 | 384 | 4% | 3 to 5% | 3 to 5% |
| react-ssr | varied | 184 | 169 | 9% | 7 to 10% | 4 to 11% |
| preact-ssr | fixed | 173 | 168 | 3% | 2 to 4% | 2 to 4% |
| preact-ssr | varied | 63.0 | 55.9 | 11% | 10 to 12% | 8 to 15% |
| webhook | fixed | 25.5 | 20.9 | 18% | 17 to 20% | 16 to 19% |
| webhook | varied | 47.3 | 39.6 | 16% | 15 to 17% | 14 to 18% |

## Cold disk

Before each of these processes the driver dropped from the page cache the Bun executable, the app and the artifact, so all three come from storage. Each repetition runs 3 such pairs, so a cell is the median of 45 processes per mode.

Startup plus request 1:

| case | branch | without | with | faster | 95% interval | runs |
|---|---|---|---|---|---|---|
| hono | fixed | not comparable, see below | | | | |
| hono | varied | 424 | 440 | 4% slower | 3 to 4% slower | 3 to 4% slower |
| elysia | fixed | 463 | 470 | 1% slower | 1 to 2% slower | 1 to 2% slower |
| elysia | varied | 463 | 477 | 3% slower | 3% slower | 3% slower |
| react-ssr | fixed | 597 | 632 | 6% slower | 5 to 6% slower | 4 to 6% slower |
| react-ssr | varied | 596 | 615 | 3% slower | 2 to 3% slower | 1 to 4% slower |
| preact-ssr | fixed | 580 | 569 | 2% | 2% | 2% |
| preact-ssr | varied | 580 | 552 | 5% | 5% | 5% |
| webhook | fixed | 428 | 443 | 4% slower | 3 to 4% slower | 3 to 4% slower |
| webhook | varied | 429 | 445 | 4% slower | 4% slower | 3 to 4% slower |

hono's fixed branch, the first case of every repetition, does not measure JITCache: in all fifteen repetitions the first and third cold process without JITCache started in about 135 ms and the second in about 417 ms, and the processes with JITCache took the mirror image (about 429, 128 and 429 ms). Every second cold process paid about 290 ms whichever mode it ran, so the median says which mode drew more of those slots. The exit of a cold process is left out for the same reason: with and without JITCache it took 0.76 to 1.05 s, which points at the cold procedure rather than at either mode.
