// jitcache-host: bun
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --jitcache-delta-at-exit; ConsumerProducer
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitCacheImportModuleAnalysis=0

// A module's analysis travels with its body. The graph under resources/module-analysis/, whose root is ma-root.mjs,
// holds one form of an analysis per module, with json and the host-defined types text and toml as its attributes. The
// Producer evaluates every module of the graph, the deferred ones when this script reads their namespaces, so its delta
// at exit captures each with its analysis. In an importing run every module's provider takes the analysis from its
// body: analysisImports grows by the graph's module count across the import, nothing misses, and Bun builds each record
// from the analysis, which the twin compares with the record the analysis parse builds. With
// jitCacheImportModuleAnalysis off the Consumer imports no analysis and parses every module, while its UCBs still
// import. Every run prints each namespace's keys and values, import.meta.url relative to the scratch directory, the
// order in which the top-level awaits settled and the attribute imports, and equals the JITCache-off run.
//
// After the graph, the script imports three modules it writes into the scratch directory, the same texts in every run,
// at the widths the graph lacks: 254 and 255 names, whose ids take one byte and two, since the largest id is the
// string count plus one, and an export name of 65,536 characters, whose blob's length takes four bytes. An importing
// run builds each one's record from its analysis, and the twin compares that record with the analysis parse's.
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import ucbBun from "./resources/ucb-bun.js";
import { maDescribeRoot } from "./resources/module-analysis/ma-describe.mjs";

// The 19 JavaScript modules and the text attribute's file, which Bun loads as a JavaScript module of its own that
// exports the text; json and toml give modules with no source text.
const graphModules = 20;
const t = ucbBun.context();
const analysisOff = t.sequence === 2;

const before = t.statistics();
const root = await import("./resources/module-analysis/ma-root.mjs");
const after = t.statistics();
if (t.imports && !analysisOff)
    t.expect("the graph's import", t.delta(before, after), { analysisImports: graphModules, analysisMisses: 0, analysisAbsent: 0 });
else
    t.expect("the graph's import", t.delta(before, after), { analysisImports: 0 });

const relative = (base, url) => path.relative(base, fileURLToPath(url));
for (const line of maDescribeRoot(root, t.scratch, relative))
    console.log(line);

const names = count => Array.from({ length: count }, (_, i) => `maV${i} = ${i}`).join(", ");
const widthModules = {
    "ma-v254.mjs": `export const ${names(254)};\n`,
    "ma-v255.mjs": `export const ${names(255)};\n`,
    "ma-long-name.mjs": `const maLong = "long";\nexport { maLong as "${"n".repeat(65536)}" };\n`,
};
for (const [name, text] of Object.entries(widthModules))
    fs.writeFileSync(t.file(name), text);
const widthBefore = t.statistics();
const [v254, v255, longName] = [
    await import(pathToFileURL(t.file("ma-v254.mjs")).href),
    await import(pathToFileURL(t.file("ma-v255.mjs")).href),
    await import(pathToFileURL(t.file("ma-long-name.mjs")).href),
];
const widthAfter = t.statistics();
t.expect("the width modules' import", t.delta(widthBefore, widthAfter), t.imports && !analysisOff ? { analysisImports: 3, analysisMisses: 0, analysisAbsent: 0 } : { analysisImports: 0 });
const sum = namespace => Object.values(namespace).reduce((total, value) => total + value, 0);
console.log(`ma-v254: ${Object.keys(v254).length} names, sum ${sum(v254)}`);
console.log(`ma-v255: ${Object.keys(v255).length} names, sum ${sum(v255)}`);
console.log(`ma-long-name: ${Object.keys(longName).map(key => `a name of ${key.length} characters`).join(", ")}, value ${Object.values(longName).join(", ")}`);
t.finish();
