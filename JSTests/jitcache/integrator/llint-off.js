// jitcache-runs: Producer; Consumer --useLLInt=false

// SPEC-integrator.md section 15.2, llint-off.js. The Producer commits loBody and loPoint at their compilations. The
// Consumer runs with the LLInt off, where every newborn CodeBlock reaches JIT::compileSync through setupJIT, so the two
// imports install there (section 7.1) and nothing compiles either body. main, which ran once in the Producer and never
// compiled there, compiles in the Consumer natively, at its one call: its single baseline compile shows that the count
// expectInstalled reads as zero for loBody and loPoint counts compileSync's compilations. The runner compares every
// output with the JITCache-off run's, and the pin comparison compares the code of the Off runs of both option sets
// (harness sub-SPEC section 11.2). With the LLInt off a function compiles at its first call, so the Off path makes no
// call that only this build makes, and its checks run only in the roles that configure JITCache.
load("./resources/integrator.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    function loBody(value, step) {
        return (value * step + 1) | 0;
    }
    function loPoint(point) {
        return point.x + point.y;
    }

    let total = 0;
    for (let i = 0; i < 40; ++i) {
        total = (total + loBody(i, 3)) | 0;
        total = (total + loPoint({ x: i, y: 2 })) | 0;
    }
    print(`loBody and loPoint added up to ${total}`);

    if (role === "Producer") {
        check(hasBody(loBody), "loBody compiled, and its key holds no committed body");
        check(hasBody(loPoint), "loPoint compiled, and its key holds no committed body");
        return;
    }
    if (role === "Consumer") {
        check(jitcacheProgress().installs >= 2, "the Consumer with the LLInt off installed fewer than two bodies");
        expectInstalled(loBody);
        expectInstalled(loPoint);
        checkSame(events(main).baselineCompiles, 1, "the baseline compiles of main, which the Producer never compiled");
    }
})(...arguments);
