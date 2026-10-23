// The worker of module-analysis-worker.mjs: it imports the graph, reads its own VM's UCB statistics around the import
// when its run configures JITCache, and posts the lines it would print and how many analyses its VM imported.
import path from "node:path";
import { fileURLToPath } from "node:url";
import { parentPort, workerData } from "node:worker_threads";
import * as jsc from "bun:jsc";
import { maDescribeRoot } from "./ma-describe.mjs";

const observes = workerData.configured && typeof jsc.jitcacheUCBStatistics === "function";
const before = observes ? jsc.jitcacheUCBStatistics() : null;
const root = await import("./ma-root.mjs");
const after = observes ? jsc.jitcacheUCBStatistics() : null;
const relative = (base, url) => path.relative(base, fileURLToPath(url));
parentPort.postMessage({
    lines: maDescribeRoot(root, workerData.scratch, relative),
    analysisImports: before && after ? after.analysisImports - before.analysisImports : null,
    analysisMisses: before && after ? after.analysisMisses - before.analysisMisses : null,
});
