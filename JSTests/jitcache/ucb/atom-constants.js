// jitcache-host: bun
// jitcache-runs: Off; Producer; Consumer; Consumer
// jitcache-runs: Off; Producer; Consumer

// SPEC-ucb.md section 13.3, atom-constants.js, F19, sections 4.2 and 7.3.3 and SPEC-ucb.codec.md E10: string constants
// longer than an inline string, compared with === and !== (whose baseline fast path needs an atom) and some also used as
// property names, in bodies captured from a generated UCB and from one Bun decoded from bytecode. Two CommonJS modules in
// resources/ hold the functions: atom-constants-a.js, which the Producer loads without Node's compile cache, so its
// bodies are captured from generated UCBs, and atom-constants-b.js, which the Producer loads with the compile cache, so
// Bun decodes it and its bodies are captured from decoded UCBs, provenance EmbedderDecoded. The Producer makes one
// decoded constant, acbDelta's, an atom through a property-key use of its own value, only after acbDelta compiled, so
// that function's image compiled the comparison against a plain string while a later capture's atom map marks the
// constant (SPEC-image.md T18). That later capture is the delta Bun makes at exit: the call that uses the constant as a
// key also gives two of acbDelta's profiles a type they never saw (an object argument and the property's int32), a
// full collection right after drains them, and the richer capture replaces the one acbDelta's compile made (THREAD
// Capture). acbDelta is still its executable's baseline CodeBlock then, so the delta can capture it: its property read
// never runs before that call, so the read's value profile, its only one, stays empty, and each baseline-to-DFG check
// it reaches by then defers (CodeBlock::shouldOptimizeNowFromBaseline, which compiles regardless only after five).
// - The first run of each sequence, with JITCache off, writes the compile cache for both modules.
// - Sequence 0: two Consumers load both modules with the compile cache and first call the functions in opposite orders;
//   in both, each decoded function is seeded at its first call (statistics, seededDecodes), whichever provenance its
//   body has, with each constant the atom map marks made an atom before publication (verifyMatched, section 13.2).
// - Sequence 1: the Consumer loads both modules without the compile cache, so it generates where it can import: module
//   a's functions import, with every string constant register an atom (T1), and module b's never import (misses
//   Provenance, section 7.3.1 step 4b).
// Every install passes the Image lane's twin check without a skip. The results are printed in one order in every run,
// and equal the JITCache-off run.
const fs = require("node:fs");
const path = require("node:path");
const Module = require("node:module");
const { context } = require("./resources/ucb-bun.js");

const t = context();
const cacheDirectory = t.file("atom-constants-compile-cache");
const marker = t.file("atom-constants.cache-written");
const load = name => require(path.join(__dirname, "resources", name));

if (t.off && !fs.existsSync(marker)) {
    Module.enableCompileCache(cacheDirectory);
    load("atom-constants-a.js");
    load("atom-constants-b.js");
    Module.flushCompileCache();
    fs.writeFileSync(marker, "the first run wrote the compile cache");
    console.log("the first run wrote the compile cache");
    process.exit(0);
}

// Which of sequence 0's Consumers this is, so the second calls the functions in the other order.
let consumerIndex = 0;
if (t.role === "Consumer") {
    const counter = t.file("atom-constants.consumers");
    consumerIndex = fs.existsSync(counter) ? Number(fs.readFileSync(counter, "utf8")) : 0;
    fs.writeFileSync(counter, String(consumerIndex + 1));
}

let a;
let b;
if (t.role === "Producer") {
    a = load("atom-constants-a.js");
    Module.enableCompileCache(cacheDirectory);
    b = load("atom-constants-b.js");
} else if (t.sequence === 0) {
    Module.enableCompileCache(cacheDirectory);
    a = load("atom-constants-a.js");
    b = load("atom-constants-b.js");
} else {
    a = load("atom-constants-a.js");
    b = load("atom-constants-b.js");
}

const inputs = ["atom-constants-a-alpha-long", "atom-constants-a-beta-long", "atom-constants-a-gamma-long", "atom-constants-b-alpha-long",
    "atom-constants-b-beta-long", "atom-constants-b-delta-long", ["atom-constants", "b", "delta", "long"].join("-"), "other"];
const calls = {
    acaEqualsAlpha: s => a.acaEqualsAlpha(s),
    acaNotEqualsBeta: s => a.acaNotEqualsBeta(s),
    acaBranchGamma: s => a.acaBranchGamma(s),
    acaKeyedAlpha: s => a.acaKeyedAlpha({ [s]: s.length }),
    acbEqualsAlpha: s => b.acbEqualsAlpha(s),
    acbNotEqualsBeta: s => b.acbNotEqualsBeta(s),
    acbDelta: s => b.acbDelta(s, null),
};
// The first calls go through functions that run once, so that no body of this script, which the Producer ran hot,
// imports inside a window.
const first = inputs[0];
const firstCalls = {
    acaEqualsAlpha: () => a.acaEqualsAlpha(first),
    acaNotEqualsBeta: () => a.acaNotEqualsBeta(first),
    acaBranchGamma: () => a.acaBranchGamma(first),
    acaKeyedAlpha: () => a.acaKeyedAlpha({ [first]: first.length }),
    acbEqualsAlpha: () => b.acbEqualsAlpha(first),
    acbNotEqualsBeta: () => b.acbNotEqualsBeta(first),
    acbDelta: () => b.acbDelta(first, null),
};
const names = Object.keys(calls).sort();
const firstCallOrder = consumerIndex === 1 ? [...names].reverse() : names;

const results = { };
for (const name of firstCallOrder) {
    const fromB = name.startsWith("acb");
    let expectations = { };
    if (t.imports && t.sequence === 0)
        expectations = { seededDecodes: 1, imports: 0 };
    else if (t.imports && !fromB)
        expectations = { imports: 1 };
    else if (t.imports)
        expectations = { imports: 0, seededDecodes: 0, "misses.Provenance": 1 };
    results[name] = [t.window(`${name}'s first call`, firstCalls[name], expectations)];
}
for (let round = 0; round < 400; ++round) {
    for (const name of names) {
        const value = calls[name](inputs[round % inputs.length]);
        if (round < inputs.length)
            results[name].push(value);
    }
}
// The Producer makes the delta constant an atom now, after acbDelta compiled, and the full collection drains what this
// call taught acbDelta's profiles, so the delta at exit captures it again with the constant marked. Every role reads
// the property and collects alike.
const keyed = b.acbDelta("", { "atom-constants-b-delta-long": 7 });
Bun.gc(true);
console.log(names.map(name => `${name}: ${results[name].join(" ")}`).join("\n"));
console.log(`keyed ${keyed}`);
t.finish();
