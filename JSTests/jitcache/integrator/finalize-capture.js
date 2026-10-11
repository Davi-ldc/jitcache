// jitcache-runs: Producer; Consumer

// SPEC-integrator.md section 15.2, finalize-capture.js. The Producer runs no delta, so every body it commits is the
// capture at the end of its baseline compilation (section 8.5). It drives the corpus of resources/corpus.js, whose bodies
// each compile once, and then finds a committed body at every body's key, with no delta run. The Consumer imports each
// body and installs it at the body's first call, so its event counts (harness sub-SPEC section 10.2) show no LLInt
// instruction and no baseline compile, where the JITCache-off run, like the Producer, shows both. The runner compares
// every output with the JITCache-off run's.
load("./resources/integrator.js", "caller relative");
load("./resources/corpus.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const corpus = integratorCorpus();
    print(`the corpus adds up to ${corpus.total}`);

    if (role === "Producer") {
        const progress = jitcacheProgress();
        checkSame(progress.deltaRuns, 0, "the Producer's delta runs");
        check(progress.capturesCommitted >= corpus.bodies.length, `the Producer committed ${progress.capturesCommitted} captures for ${corpus.bodies.length} bodies`);
        for (const [fn, kind] of corpus.bodies) {
            expectCompiledNatively(fn, kind);
            check(hasBody(fn, kind), `${label(fn, kind)} compiled, and its key holds no committed body`);
        }
        return;
    }

    if (role === "Consumer") {
        const progress = jitcacheProgress();
        check(progress.installs >= corpus.bodies.length, `the Consumer installed ${progress.installs} bodies of the ${corpus.bodies.length} the Producer compiled`);
        for (const [fn, kind] of corpus.bodies)
            expectInstalled(fn, kind);
        return;
    }

    if (hasTwins()) {
        for (const [fn, kind] of corpus.bodies)
            expectCompiledNatively(fn, kind);
    }
})(...arguments);
