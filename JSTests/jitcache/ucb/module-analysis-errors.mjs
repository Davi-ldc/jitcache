// jitcache-host: bun

// Link errors that a record built from an imported analysis must raise as the analysis parse's record does. The script
// first imports the modules the failing ones depend on (module-analysis/ma-err-exporter.mjs and ma-err-stars.mjs with its
// two star exports), so the Producer's delta at exit captures them with their analyses and an importing run builds their
// records from those. Then it imports, one at a time, a module that imports a binding its source lacks, one that imports
// a name two star exports make ambiguous, and a TypeScript module that re-exports a name its source lacks, which the
// JITCache-off run rejects because the parse's record keeps its TypeScript flag clear. The first two never link, so
// neither runs and no producer captures either: an importing run misses its analysis and parses it. The TypeScript
// module's text links in the Producer from a copy in the scratch directory, beside an exporter that has the name, so
// the Producer captures its body with its analysis. Keys come from the text, so an importing run builds the TypeScript
// module's record from that analysis, and the link fails only if that record keeps the TypeScript flag clear. Each
// error prints with its name and message, which equal the JITCache-off run's.
import fs from "node:fs";
import { pathToFileURL } from "node:url";
import ucbBun from "./resources/ucb-bun.js";

const t = ucbBun.context();

let before = t.statistics();
const exporter = await import("./module-analysis/ma-err-exporter.mjs");
const stars = await import("./module-analysis/ma-err-stars.mjs");
let after = t.statistics();
t.expect("the sources' imports", t.delta(before, after), t.imports ? { analysisImports: 4, analysisMisses: 0, analysisAbsent: 0 } : { analysisImports: 0 });
let total = 0;
for (let i = 0; i < 1000; ++i)
    total = (total + exporter.maErrHot(i)) | 0;
console.log(`sources ${exporter.maPresent} ${stars.maOnlyA} ${Object.keys(stars).join(",")} ${total}`);

// The copy prints nothing, so the Producer's output stays the JITCache-off run's.
if (t.role === "Producer") {
    const reexportCopy = t.file("ma-err-reexport.ts");
    fs.writeFileSync(t.file("ma-err-exporter.mjs"), "export const maLacking = \"lacking\";\n");
    fs.writeFileSync(reexportCopy, fs.readFileSync(new URL("./module-analysis/ma-err-reexport.ts", import.meta.url)));
    await import(pathToFileURL(reexportCopy).href);
}

const cases = [
    ["./module-analysis/ma-err-missing.mjs", { analysisImports: 0, analysisMisses: 1 }],
    ["./module-analysis/ma-err-ambiguous.mjs", { analysisImports: 0, analysisMisses: 1 }],
    ["./module-analysis/ma-err-reexport.ts", { analysisImports: 1, analysisMisses: 0, analysisAbsent: 0 }],
];
for (const [specifier, expectations] of cases) {
    before = t.statistics();
    try {
        await import(specifier);
        console.log(`${specifier} linked`);
    } catch (error) {
        console.log(`${specifier} ${error?.name}: ${error?.message}`);
    }
    after = t.statistics();
    t.expect(specifier, t.delta(before, after), t.imports ? expectations : { analysisImports: 0 });
}
t.finish();
