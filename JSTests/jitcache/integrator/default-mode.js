// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer --jitcache-strict=0; Consumer --jitcache-strict=0
// jitcache-check: 0:0 resources/strict-on.ts
// jitcache-check: 0:1 resources/strict-on.ts
// jitcache-check: 1:0 resources/strict-off.ts
// jitcache-check: 1:1 resources/strict-off.ts

// SPEC-integrator.md section 15.2, default-mode.js. The corpus of finalize-capture.js runs in two sequences: the first
// with strict on, as the runner's --jitcache-strict=1 gives every run, and the second with --jitcache-strict=0, which
// overrides it in every run and is the default the bench measures (THREAD Verification). The checkers read each run's
// final status and confirm the strictness it ran with. In both sequences the Producer compiles every body of the corpus
// and nothing else, since the corpus's loop keeps every caller in the LLInt, and commits each at its compilation; each
// Consumer installs exactly those bodies, so the two modes install the same bodies, and the runner compares every
// output with the JITCache-off run's.
load("./resources/integrator.js", "caller relative");
load("./resources/corpus.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const corpus = integratorCorpus();
    print(`the corpus adds up to ${corpus.total}`);

    if (role === "Producer") {
        for (const [fn, kind] of corpus.bodies)
            check(hasBody(fn, kind), `${label(fn, kind)} compiled, and its key holds no committed body`);
        return;
    }

    if (role === "Consumer") {
        checkSame(jitcacheProgress().installs, corpus.bodies.length, "the bodies the Consumer installed");
        for (const [fn, kind] of corpus.bodies)
            expectInstalled(fn, kind);
    }
})(...arguments);
