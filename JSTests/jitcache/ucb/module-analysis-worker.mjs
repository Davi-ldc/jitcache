// jitcache-host: bun

// A worker's VM imports the analyses of the modules its process's main VM captured. This script imports the graph of
// module-analysis.mjs (resources/module-analysis/ma-root.mjs), so the Producer's main VM captures every module of it at
// its delta at exit, then starts a worker (resources/module-analysis/ma-worker.mjs) that imports the same graph; in the
// Producer that worker falls back to a Consumer of the main VM's artifact. In the Consumer the worker's VM imports the
// analysis of every module of the graph, as its own statistics show, and the runner checks that worker's twin report as
// it checks the main VM's. Every run, the worker's lines included, equals the JITCache-off run.
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Worker } from "node:worker_threads";
import ucbBun from "./resources/ucb-bun.js";
import { maDescribeRoot } from "./resources/module-analysis/ma-describe.mjs";

// The modules of the graph that carry an analysis, as module-analysis.mjs counts them.
const graphModules = 20;
const t = ucbBun.context();
const relative = (base, url) => path.relative(base, fileURLToPath(url));

const root = await import("./resources/module-analysis/ma-root.mjs");
for (const line of maDescribeRoot(root, t.scratch, relative))
    console.log(`main ${line}`);

const worker = new Worker(new URL("./resources/module-analysis/ma-worker.mjs", import.meta.url), {
    workerData: { configured: t.configured, scratch: t.scratch },
});
const result = await new Promise((resolve, reject) => {
    worker.once("message", resolve);
    worker.once("error", reject);
});
await new Promise(resolve => worker.once("exit", resolve));
for (const line of result.lines)
    console.log(`worker ${line}`);
if (t.imports && t.observes) {
    t.check(result.analysisImports === graphModules && result.analysisMisses === 0,
        () => `the worker's VM imported ${result.analysisImports} analyses and missed ${result.analysisMisses}, expected ${graphModules} and 0`);
}
t.finish();
