// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer; ConsumerProducer
// jitcache-runs: Producer; ConsumerProducer --jitcache-delta-at-exit; Consumer
// jitcache-expect-no-install: 1:1

// SPEC-ucb.md section 13.3, role-keys.js: a "use strict" program, an indirect eval of "use strict" text and a sloppy
// function's direct eval of "use strict" text. A request keys its body from the features its request object snapshots
// before the parse sets the directive's strict bit (section 7.1, F18), so a body one role captured imports in every
// other role, whether the capturing run generated the body or the importing run will:
// - sequence 0: a Producer captures them, and they import in a Consumer and in a ConsumerProducer (statistics);
// - sequence 1: a Producer only creates the artifact, a ConsumerProducer generates the bodies, which miss, and commits
//   them, so it has nothing to install, and a Consumer imports them.
// The Bun variant, a program evaluated after vm.compileFunction left a global scope extension, is role-keys-bun.js.
//
// The Producer of sequence 1 skips the bodies, so the script prints only what every run prints and checks the results
// itself; the bodies leave no binding behind.
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const skipsBodies = t.sequence === 1 && t.role === "Producer";
    const generates = t.sequence === 1 && t.role === "ConsumerProducer";

    function sloppyDirectEval(text)
    {
        return eval(text);
    }

    const cases = [
        ["a \"use strict\" program", loadString, `"use strict"; { let total = 0; for (let i = 0; i < 3000; ++i) total = (total + i * 3) | 0; total; }`, 13495500],
        ["an indirect eval of \"use strict\" text", text => (0, eval)(text), `"use strict"; { let total = 1; for (let i = 0; i < 3000; ++i) total = (total + i * 5) | 0; total; }`, 22492501],
        ["a sloppy function's direct eval of \"use strict\" text", sloppyDirectEval, `"use strict"; { let total = 2; for (let i = 0; i < 3000; ++i) total = (total + i * 9) | 0; total; }`, 40486502],
    ];

    if (!skipsBodies) {
        for (const [label, evaluate, text, expected] of cases) {
            const before = t.statistics();
            const value = evaluate(text);
            const after = t.statistics();
            t.check(value === expected, `${label} gave ${value}, expected ${expected}`);
            if (generates)
                t.expect(label, t.delta(before, after), { imports: 0, "misses.NoBody": [">=", 1] });
            else if (t.imports)
                t.expect(label, t.delta(before, after), { imports: [">=", 1] });
        }
    }
    print("every body keys alike in every role");
    t.finish();
})(...arguments);
