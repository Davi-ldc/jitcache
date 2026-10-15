// jitcache-host: bun

// vm.Script with cachedData, whose decode has no request point of its own.
// - new vm.Script(text, { cachedData }): Bun's constructScript first asks NodeVMScript::unlinkedCodeBlockFor for the
//   program's UCB, so that a SyntaxError throws at construction as it does in Node. That call goes through
//   CodeCache::getUnlinkedProgramCodeBlock, the program request point, where the Consumer imports the program's body.
//   Only then does constructScript decode the cachedData with Bun's own decode (JSC::decodeCodeBlock, outside the
//   CodeCache) and compile a CodeBlock of the decoded block on an executable no run uses. The decoded block gets no
//   record: the window counts no Decoded record, seeded decode or attach, and no record but the import's (statistics).
// - runInThisContext makes no request: NodeVMScript::evaluate hands the UCB the construction requested to
//   JSC::evaluate, whose ProgramExecutable::initializeGlobalProperties takes it in place of the CodeCache lookup, so
//   the run never uses the decoded block, and the imported UCB's first CodeBlock installs its body (statistics,
//   jitcacheProgress installs).
// The cachedData comes from one Script with produceCachedData, made by the first run and kept in the sequence's scratch
// directory, so every later run, the oracle's included, decodes the same bytes; cachedDataRejected and the result are
// the same in every run. That first run is the Producer, whose producing Script requests the program before the
// window, so there the construction is a CodeCache hit that records nothing.
//
// The runs use the lane's default sequence; Bun calls delta at exit for the Producer.
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

// An importing run imports the program at the construction's request and installs it in the run. Each window holds
// bare statements, never a function's first call, which would make a request of its own inside it; for the same
// reason the run's window opens after t.progress()'s first call.
const imported = t.imports ? 1 : 0;
const before = t.statistics();
const script = new vm.Script(text, { cachedData });
const constructed = t.statistics();
t.expect("new vm.Script with cachedData", t.delta(before, constructed), {
    "records.Generated": 0, "records.Decoded": 0, "records.Imported": imported, imports: imported,
    seededDecodes: 0, attaches: 0,
});
console.log(`cachedDataRejected ${script.cachedDataRejected}`);

const installsBefore = t.progress()?.installs;
const running = t.statistics();
const result = script.runInThisContext();
const ran = t.statistics();
const installsAfter = t.progress()?.installs;
t.expect("runInThisContext", t.delta(running, ran), {
    "records.Generated": 0, "records.Decoded": 0, "records.Imported": 0, imports: 0, seededDecodes: 0, attaches: 0,
});
if (installsBefore !== undefined) {
    const installed = installsAfter - installsBefore;
    t.check(installed === imported, () => `runInThisContext installed ${installed} bodies, expected ${imported}`);
}
console.log(`result ${result}`);
t.finish();
