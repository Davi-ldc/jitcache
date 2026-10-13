# JITCache macro benches

## Reproducing

Each case in `apps/` is a small app, `requests/` holds the requests one run sends it, and `Tools/Scripts/jitcache-macro-bench.ts` runs every case in fresh Bun processes, with JITCache off and on, and reports how long each process takes to finish.

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

The results go to `jitcache-macro/<time>/` next to the Bun binary, or to `--out=<directory>`: `summary.md` and `results.jsonl`. `--cases=` picks cases, `--processes=` sets the processes per mode (5) and `--passes=` how many times each process sends its requests (34). One case runs by hand with `NODE_ENV=production bun run.ts <case> --passes=N` from this directory. The PDF inputs are public domain; their sources are in `inputs/pdf/SOURCES.md`.
