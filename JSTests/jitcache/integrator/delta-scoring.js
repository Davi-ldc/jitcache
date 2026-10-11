// jitcache-runs: Producer; Consumer

// SPEC-integrator.md section 15.2, delta-scoring.js. dsBody compiles at its 34th call, and the Producer commits it at
// that compilation with the LLInt phase's feedback and cold ICs. More warm-up then passes doubles, which give its value
// profiles new categories, and fills its property ICs; a full collection folds the pending samples into the predictions,
// which capture reads (THREAD Capture). The first jitcacheDelta() therefore commits a richer body for dsBody's key: its
// committedBodies counts it, and the key's cb.summary changes. A second jitcacheDelta() right after it, with no
// JavaScript run in between and main still in the LLInt, finds nothing that beats the kept summaries and commits nothing,
// and dsBody's file stays as the first delta left it. The Consumer imports that last commit, whose cb.summary the
// Producer saved in the scratch directory. The runner compares every output with the JITCache-off run's.
load("./resources/integrator.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    function dsBody(point, scale) {
        return point.x * scale + point.y;
    }

    let total = 0;
    for (let i = 0; i < 40; ++i)
        total += dsBody({ x: i, y: 1 }, 2);
    const atCompilation = role === "Producer" ? hex(sectionOf(dsBody, "call", "cb.summary")) : null;
    for (let i = 0; i < 10; ++i)
        total += dsBody({ x: i + 0.5, y: 1 }, 2);
    print(`dsBody added up to ${total}`);

    if (role === "Producer") {
        check(atCompilation !== null, "dsBody compiled, and its key holds no body committed at its compilation");
        fullGC();
        const first = jitcacheDelta();
        const second = jitcacheDelta();
        const afterFirst = hex(sectionOf(dsBody, "call", "cb.summary"));
        check(first.committedBodies >= 1, `the first delta committed ${first.committedBodies} bodies after more warm-up`);
        check(afterFirst !== atCompilation, "the first delta left dsBody's body as its compilation committed it");
        checkSame(second.committedBodies, 0, "the bodies a delta with nothing new committed");
        check(second.eligibleKeys >= 1, `the second delta found ${second.eligibleKeys} eligible keys`);
        checkSame(hex(sectionOf(dsBody, "call", "cb.summary")), afterFirst, "dsBody's cb.summary after a delta with nothing new");
        saveText(scratch, "delta-scoring.summary", afterFirst);
        return;
    }

    if (role === "Consumer") {
        expectInstalled(dsBody);
        checkSame(hex(sectionOf(dsBody, "call", "cb.summary")), loadText(scratch, "delta-scoring.summary"), "the cb.summary of the body the Consumer found");
    }
})(...arguments);
