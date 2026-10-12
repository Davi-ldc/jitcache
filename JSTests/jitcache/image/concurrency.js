// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=true; Consumer --useConcurrentJIT=true

// SPEC-image.md T14: T1's programs with concurrent JIT on. Each loaded script's directives are comments here, and its
// body runs as it does alone, so the producer records on JIT worklist threads and attaches each record to its code before
// finalization, and the consumer imports what the finalize captures and the delta at exit committed. module.mjs, a
// module, is imported once the scripts have run, and prints after them. A twin check skips every import of these runs
// (section 11.3), which the runner lists since the runs turn concurrency on, and the oracle compares their output and
// heap as in T1.
(function main(role, scratch, artifact, sequence) {
    const corpus = ["arithmetic.js", "calls.js", "exceptions.js", "iteration.js", "properties.js", "scopes.js", "stack-overflow.js", "strict-equality.js", "switches.js"];
    for (const script of corpus)
        load(`./${script}`, "caller relative");
    import("./module.mjs").catch(error => print(`module.mjs failed: ${error}`));
})(...arguments);
