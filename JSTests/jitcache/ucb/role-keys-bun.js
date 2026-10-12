// jitcache-host: bun
// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer; ConsumerProducer
// jitcache-runs: Producer; ConsumerProducer --jitcache-delta-at-exit; Consumer
// jitcache-expect-no-install: 1:1

// SPEC-ucb.md section 13.3, role-keys.js, the variant marked Bun: a program evaluated after vm.compileFunction has left
// its context's global scope extension set (F11), so its request carries the with-scope bit, as with-scope.js describes.
// Its body keys alike in every role: a Producer's capture imports in a Consumer and in a ConsumerProducer (sequence 0),
// and a ConsumerProducer that generated it commits a body a Consumer imports (sequence 1), as in role-keys.js.
const vm = require("node:vm");
const { context } = require("./resources/ucb-bun.js");

const t = context();
const skipsBody = t.sequence === 1 && t.role === "Producer";
const generates = t.sequence === 1 && t.role === "ConsumerProducer";

if (!skipsBody) {
    // compileFunction installs a global scope extension on the main context's global object and never removes it.
    vm.compileFunction("return 1;")();
    const text = `"use strict"; { let total = 4; for (let i = 0; i < 3000; ++i) total = (total + i * 7) | 0; total; }`;
    const before = t.statistics();
    const value = vm.runInThisContext(text);
    const after = t.statistics();
    t.check(value === 31489504, `the program gave ${value}`);
    if (generates)
        t.expect("the program under a scope extension", t.delta(before, after), { imports: 0, "misses.NoBody": [">=", 1] });
    else if (t.imports)
        t.expect("the program under a scope extension", t.delta(before, after), { imports: [">=", 1] });
}
console.log("the program under a scope extension keys alike in every role");
t.finish();
