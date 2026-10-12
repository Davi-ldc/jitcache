// jitcache-host: bun

// SPEC-ucb.md section 13.3, vm-script-cached-data.js, F11 and section 7.2.5: `new vm.Script(text, { cachedData })`
// decodes the program with Bun's own decode (NodeVMScript's constructor, outside the CodeCache), so the block it yields
// has no record and nothing JITCache-related happens there (statistics unchanged), while runInThisContext runs a
// ProgramExecutable that goes through CodeCache::getUnlinkedProgramCodeBlock, the program request point, where the
// Consumer imports the program's body (statistics). The cachedData comes from one Script with produceCachedData, made
// by the first run and kept in the sequence's scratch directory, so every later run, the oracle's included, decodes the
// same bytes; cachedDataRejected and the result are the same in every run.
//
// The runs use the lane's default sequence (SPEC-integrator.harness.md section 7.4); Bun calls delta at exit for the
// Producer.
const fs = require("node:fs");
const vm = require("node:vm");
const { context } = require("./resources/ucb-bun.js");

const t = context();
const text = "{ let vmScriptTotal = 0; for (let i = 0; i < 3000; ++i) vmScriptTotal = (vmScriptTotal + i * 9) | 0; vmScriptTotal; }";

const cachedDataFile = t.file("vm-script-cached-data.bin");
if (!fs.existsSync(cachedDataFile)) {
    const producing = new vm.Script(text, { produceCachedData: true });
    t.check(producing.cachedDataProduced === true && producing.cachedData, "vm.Script produced no cachedData");
    fs.writeFileSync(cachedDataFile, producing.cachedData);
}
const cachedData = fs.readFileSync(cachedDataFile);

const before = t.statistics();
const script = new vm.Script(text, { cachedData });
const constructed = t.statistics();
t.expect("new vm.Script with cachedData", t.delta(before, constructed), {
    "records.Generated": 0, "records.Decoded": 0, "records.Imported": 0, imports: 0, seededDecodes: 0, attaches: 0,
});
console.log(`cachedDataRejected ${script.cachedDataRejected}`);

const result = script.runInThisContext();
const ran = t.statistics();
if (t.imports)
    t.expect("runInThisContext", t.delta(constructed, ran), { imports: [">=", 1] });
console.log(`result ${result}`);
t.finish();
