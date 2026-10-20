// jitcache-host: bun
// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer
// jitcache-expect-no-install: 1:1

// Bun's vm.compileFunction, a path with no request point of its own.
// - Without cachedData, constructAnonymousFunction asks CodeCache::getUnlinkedProgramCodeBlock for the wrapper program,
//   so the wrapper comes through the program request point and is recorded there; it never runs, so no body holds it.
//   The user function and its construct body import at their first call and first `new` in the Consumer (statistics).
// - With cachedData, Bun decodes the wrapper itself, outside the CodeCache: the wrapper and the user function get no
//   record, nothing in them imports or is captured, and cachedDataRejected is the same in every run. The user function's
//   body is decoded at its first call from a copy of the cachedData that the payload owns, so every run calls it.
// Sequence 0 runs both cases; sequence 1 runs the cachedData case alone, so its Consumer has nothing to install.
// The cachedData comes from one compile with produceCachedData, made by the first run that needs it and kept in the
// sequence's scratch directory, so every later run, the oracle's included, decodes the same bytes. Every run prints the
// same output, which the oracle compares with the JITCache-off run.
const fs = require("node:fs");
const vm = require("node:vm");
const { context } = require("./resources/ucb-bun.js");

const t = context();

const generatedBody = "if (new.target) { this.value = (a * 3 + 1) | 0; return; } let total = a; for (let i = 0; i < 4; ++i) total = (total * 5 + i) | 0; return total;";
const cachedBody = "if (new.target) { this.value = (a * 7 + 2) | 0; return; } let total = a; for (let i = 0; i < 4; ++i) total = (total * 11 + i) | 0; return total;";

if (t.sequence === 0) {
    const before = t.statistics();
    const generated = vm.compileFunction(generatedBody, ["a"]);
    const compiled = t.statistics();
    if (t.imports) {
        t.expect("the wrapper's program request", t.delta(before, compiled), {
            "records.Generated": [">=", 1], imports: 0, "misses.NoBody": [">=", 1],
        });
    }
    let called = t.window("the user function's first call", () => generated(1), t.imports ? { imports: [">=", 1] } : { });
    let constructed = t.window("the user function's first construction", () => new generated(1).value, t.imports ? { imports: [">=", 1] } : { });
    for (let i = 0; i < 400; ++i) {
        called = (called + generated(i)) | 0;
        constructed = (constructed + new generated(i).value) | 0;
    }
    console.log(`without cachedData: ${called} ${constructed}`);
}

const cachedDataFile = t.file("compile-function.cached-data");
if (!fs.existsSync(cachedDataFile)) {
    const producing = vm.compileFunction(cachedBody, ["a"], { produceCachedData: true });
    t.check(producing.cachedDataProduced === true && producing.cachedData, "compileFunction produced no cachedData");
    fs.writeFileSync(cachedDataFile, producing.cachedData);
}
const cachedData = fs.readFileSync(cachedDataFile);

const before = t.statistics();
const decoded = vm.compileFunction(cachedBody, ["a"], { cachedData });
const compiled = t.statistics();
t.expect("compileFunction with cachedData", t.delta(before, compiled), {
    "records.Generated": 0, "records.Imported": 0, imports: 0, attaches: 0,
});
console.log(`cachedDataRejected ${decoded.cachedDataRejected}`);
// The window holds the calls alone: the helpers' first calls above (t.delta, t.expect) and console.log's first use
// generate bodies of their own.
let called = 0;
let constructed = 0;
const calling = t.statistics();
for (let i = 0; i < 400; ++i) {
    called = (called + decoded(i)) | 0;
    constructed = (constructed + new decoded(i).value) | 0;
}
const after = t.statistics();
t.expect("the decoded user function's calls", t.delta(calling, after), {
    "records.Generated": 0, "records.Imported": 0, imports: 0, attaches: 0,
});
console.log(`with cachedData: ${called} ${constructed}`);
t.finish();
