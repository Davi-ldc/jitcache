// jitcache-host: bun
// jitcache-runs: Producer --jitcache-delta-at-exit --jitcache-delta-capture=compiled; Consumer
// The Producer captures compiled bodies only: the windows count the requests of the bodies under test, and with the
// delta capture all each window's own action, which ran in the Producer, would have a body too (run-once-capture.js
// covers it).

// Builtin roots, whose identity is recorded at their creation and whose source digest comes from their provider, never
// from their text.
// - JSC builtins. BuiltinExecutables keeps one UFE per builtin and VM, which the VM's first global object creates before
//   any script runs, so a later global object creates no builtin root while that cache holds it. Bun.shrink() empties
//   the cache (VM::deleteAllCode, which runs once the VM is idle), so the global object vm.createContext() creates next
//   makes each builtin it installs again, from generated BuiltinSourceMetadata: that creation computes no source digest
//   (sourceDigests unchanged) and takes one from the metadata for each root (suppliedSourceDigests grows). A builtin's
//   body imports at its first call in the Consumer (statistics): Array.prototype.findLastIndex's, the one import of its
//   window, since it calls only native functions and intrinsics besides a callback whose own first call comes before
//   the window.
// - Bun internal modules created from their text, as a `bun` run creates them: loading node:querystring records its
//   root once and takes its digest from its provider, the builtins section's table (statistics: a root, a supplied
//   digest, no digest computed from text), and its functions import at their first call in the Consumer.
// Each window makes its first calls through a function that runs once, so that no body of this script, which its
// Producer ran hot, imports inside it: only the builtin's and the internal module's bodies can. The output equals the
// JITCache-off run.
const vm = require("node:vm");
const { context } = require("./resources/ucb-bun.js");

const t = context();

// The code deletion runs when this script's evaluation returns, before the timer.
Bun.shrink();
setTimeout(() => {
    const contexts = [];
    t.window("creating a global object once the builtin cache is empty", () => contexts.push(vm.createContext({ })), {
        sourceDigests: 0, suppliedSourceDigests: [">=", 1],
    });
    contexts.length = 0;

    const values = [3, 9, 4, 12, 7];
    const isLarge = value => value > 5;
    // isLarge's first call, which imports it in the Consumer, comes before the window.
    isLarge(values[0]);
    let found = t.window("Array.prototype.findLastIndex's first call", () => values.findLastIndex(isLarge), t.imports ? { imports: 1 } : { });
    for (let i = 0; i < 400; ++i)
        found = (found + values.findLastIndex(isLarge)) | 0;
    console.log(`findLastIndex ${found}`);

    const querystring = t.window("loading node:querystring", () => require("node:querystring"), t.configured ? {
        "counts.roots": [">=", 1], suppliedSourceDigests: [">=", 1], sourceDigests: 0,
    } : { });
    const inputs = ["a=1&b=two", "x=%20y&x=z", "plain", "k=v&k=w&q"];
    // The wrappers in `calls` run hot, so the windows call the module's functions through `firstCalls`, which run once.
    const calls = {
        escape: s => querystring.escape(s),
        parse: s => JSON.stringify(querystring.parse(s)),
    };
    const firstCalls = {
        escape: () => querystring.escape(inputs[0]),
        parse: () => JSON.stringify(querystring.parse(inputs[0])),
    };
    for (const name of ["escape", "parse"]) {
        const first = t.window(`querystring.${name}'s first call`, firstCalls[name], t.imports ? { imports: [">=", 1] } : { });
        let total = first.length;
        for (let i = 0; i < 400; ++i)
            total = (total + calls[name](inputs[i % inputs.length]).length) | 0;
        console.log(`querystring.${name} ${first} ${total}`);
    }
    t.finish();
}, 0);
