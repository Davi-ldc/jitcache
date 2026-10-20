// jitcache-host: bun
// jitcache-runs: Producer --jitcache-delta-at-exit --jitcache-delta-capture=all; Consumer

// Modules whose top level runs once without compiling carry their analysis. Under the delta capture all, the Producer's
// delta at exit captures module-analysis/ma-uncompiled-a.mjs and the module it imports without an image, each with the
// analysis its parse built (analysisCaptured, which no script can read after that delta), and the Consumer imports both
// analyses from those bodies. A function every run calls often enough to compile gives the Consumer a body to install.
// Every run equals the JITCache-off run.
import ucbBun from "./resources/ucb-bun.js";
import { maMissHot } from "./module-analysis/ma-miss-hot.mjs";

const t = ucbBun.context();

let total = 0;
for (let i = 0; i < 1000; ++i)
    total = (total + maMissHot(i)) | 0;
console.log(`hot ${total}`);

const before = t.statistics();
const uncompiled = await import("./module-analysis/ma-uncompiled-a.mjs");
const after = t.statistics();
t.expect("the modules' import", t.delta(before, after), t.imports ? { analysisImports: 2, analysisMisses: 0, analysisAbsent: 0 } : { analysisImports: 0 });
console.log(`uncompiled ${uncompiled.maUncompiledA}`);
t.finish();
