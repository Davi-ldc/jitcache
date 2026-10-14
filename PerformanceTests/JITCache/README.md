# JITCache macro benches

## Reproducing

Each case in `apps/` is a small app and `requests/` holds its request variants. `Tools/Scripts/jitcache-macro-bench.ts` runs every case in fresh Bun processes pinned to one CPU, with JITCache off and as a consumer, and reports how long each process takes to finish.

On x86_64, from the repository root:

```sh
bun build.ts bun-ci-release
cd PerformanceTests/JITCache && bun install --frozen-lockfile && cd ../..
bun Tools/Scripts/jitcache-macro-bench.ts --bun=$HOME/collo-local/build/jitcache/linux-x86_64-ci-release/bun
```

On ARM64, build on x86_64 with `bun build.ts bun-ci-release --arch=aarch64`, then copy `PerformanceTests/JITCache/` (without `node_modules`), `Tools/Scripts/jitcache-macro-bench.ts` and the built `linux-aarch64-ci-release/bun` to the ARM64 machine, keeping the first two at the same paths under one directory. There, install a stock Bun to run the driver (`curl -fsSL https://bun.sh/install | bash`) and run:

```sh
cd PerformanceTests/JITCache && bun install --frozen-lockfile && cd ../..
bun Tools/Scripts/jitcache-macro-bench.ts --bun=<path to the copied bun>
```

The machine needs `taskset`, `dd`, `flock` and `who`; where `fincore` exists, it checks that the cold consumers start without the artifact in the page cache. The bench refuses to run while this directory holds a `bunfig.toml`.

The results go to `jitcache-macro/<time>/` next to the Bun binary, or to `--out=<directory>`: `results.jsonl`, `summary.md`, the artifacts, and the output of failed processes, census runs and report runs. The driver prints every flag when it gets one it cannot read. The main ones, with their defaults:

| flag | default |
|---|---|
| `--cases=` | `hono,elysia,react-ssr,preact-ssr,webhook`; `pdf` runs only when named |
| `--branches=` | `fixed,varied` |
| `--scenarios=` | `50,500`, the calls of each measured process |
| `--processes=` | `10` per mode, scenario, case and branch in each repetition |
| `--repetitions=` | `3` |
| `--producer-calls=` | `100` |
| `--cold-processes=` | `3` per scenario, case and branch in each repetition |
| `--cpu=` | the highest CPU the driver may use, where every Bun process runs |
| `--no-census`, `--no-reports` | both on: the untimed runs that count compiles by call and write JITCache's bench report |
| `--check` | off; a short correctness run without the build lock, whose times mean nothing |
| `--summarize <results.jsonl>...` | rebuilds `summary.md` from earlier results; pooling several files needs `--out` |

One case runs by hand from this directory with `NODE_ENV=production bun run.ts <case> --calls=N --branch=fixed|varied`. `--mark-calls` writes `call <n>` to standard error before each call, and `--kill-at-end` sends the process SIGKILL right after its last line. The PDF inputs are public domain; their sources are in `inputs/pdf/SOURCES.md`.
