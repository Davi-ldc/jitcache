// jitcache-runs: Off

// Harness sub-SPEC H6, with the baseline JIT on: a function called until it compiles shows baselineCompiles 1, and its
// later calls add no LLInt instruction. The integrator directory's default turns concurrent JIT off, so the compilation
// installs within the call that requests it. Any other outcome throws.
(function main() {
    function h6Warm(x) {
        return x + 1;
    }
    let counts = null;
    for (let i = 0; i < 10000 && !counts?.baselineCompiles; ++i) {
        h6Warm(i);
        counts = jitcacheBodyEvents(h6Warm, "call");
    }
    if (counts?.baselineCompiles !== 1)
        throw new Error(`h6Warm shows ${counts?.baselineCompiles} baseline compiles, expected 1`);
    const compiledAfter = counts.llintInstructions;
    for (let i = 0; i < 20; ++i)
        h6Warm(i);
    const later = jitcacheBodyEvents(h6Warm, "call");
    if (later.llintInstructions !== compiledAfter)
        throw new Error(`h6Warm's calls after its compile added ${later.llintInstructions - compiledAfter} LLInt instructions`);
    if (later.baselineCompiles !== 1)
        throw new Error(`h6Warm compiled again, ${later.baselineCompiles} baseline compiles in all`);
    print(`baselineCompiles ${later.baselineCompiles}; LLInt instructions after the compile: ${later.llintInstructions - compiledAfter}`);
})();
