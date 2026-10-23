// jitcache-host: bun
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit --jitcache-delta-capture=compiled; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer

// Modules whose analysis no body gives, each parsed as without JITCache, one case per sequence:
// 0. A module whose text the Consumer changes: the Producer captures one text, the Consumer writes another at the same
//    path, which prints the same, so the Consumer misses its analysis (analysisMisses) and parses it. The texts differ
//    in a string export, since Bun's transpiler folds constant arithmetic: `6 * 7` and `42` would give one text.
// 1. A module given an error in the Consumer: the Producer captures the module's valid text from a file of its own,
//    then every run writes a text that exports an undeclared name at another path, imports it and prints the error,
//    which equals the JITCache-off run's. The Producer cannot load both texts from one path: once Bun has loaded a path,
//    it gives a later import of that path, even under a query string, the module it loaded.
// 2. Under the delta capture compiled, a module whose top level never compiled has no body, so the Consumer misses it.
// 3. An ES module that Node's compile cache serves: the Producer loads it with module.enableCompileCache() over a
//    scratch directory and fills the cache, and the Consumer, enabling the same directory, gets its bytecode, so its
//    provider asks for no analysis and no analysis counter changes across its load.
// A function every run calls often enough to compile gives each Consumer a body to install.
import fs from "node:fs";
import Module from "node:module";
import { pathToFileURL } from "node:url";
import ucbBun from "./resources/ucb-bun.js";
import { maMissHot } from "./resources/module-analysis/ma-miss-hot.mjs";

const t = ucbBun.context();
const producer = t.role === "Producer";

let total = 0;
for (let i = 0; i < 1000; ++i)
    total = (total + maMissHot(i)) | 0;
console.log(`hot ${total}`);

// The statistics around one load; a closure makes no module request of its own, so the analysis counters see only the
// load.
async function load(label, action, expectations)
{
    const before = t.statistics();
    const value = await action();
    const after = t.statistics();
    if (t.imports)
        t.expect(label, t.delta(before, after), expectations);
    return value;
}

switch (t.sequence) {
case 0: {
    const changedPath = t.file("ma-changed.mjs");
    fs.writeFileSync(changedPath, `export const maChanged = 42;\nexport const maWriter = "${producer ? "Producer" : "a later run"}";\n`);
    const changed = await load("the changed module", () => import(pathToFileURL(changedPath).href), { analysisImports: 0, analysisMisses: 1 });
    console.log(`changed ${changed.maChanged}`);
    break;
}
case 1: {
    if (producer) {
        const validPath = t.file("ma-valid.mjs");
        fs.writeFileSync(validPath, "export const maBroken = 1;\n");
        await import(pathToFileURL(validPath).href);
    }
    const brokenPath = t.file("ma-broken.mjs");
    fs.writeFileSync(brokenPath, "export const maBroken = 1;\nexport { maNotDeclared };\n");
    try {
        await import(pathToFileURL(brokenPath).href);
        console.log("the broken module loaded");
    } catch (error) {
        console.log(`broken ${error?.name}: ${error?.message}`);
    }
    break;
}
case 2: {
    const uncompiled = await load("the uncompiled module", () => import("./resources/module-analysis/ma-miss-uncompiled.mjs"), { analysisImports: 0, analysisMisses: 1 });
    console.log(`uncompiled ${uncompiled.maUncompiled}`);
    break;
}
case 3: {
    Module.enableCompileCache(t.file("ma-compile-cache"));
    const cached = await load("the module the compile cache serves", () => import("./resources/module-analysis/ma-miss-cached.mjs"), { analysisImports: 0, analysisMisses: 0, analysisAbsent: 0 });
    if (producer)
        Module.flushCompileCache();
    console.log(`cached ${cached.maCached}`);
    break;
}
}
t.finish();
