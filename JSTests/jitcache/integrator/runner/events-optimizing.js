// jitcache-runs: Off --printEachOSRExit=true --dumpDisassembly=true --verboseOSR=true
// jitcache-check: 0 resources/optimizing-counts.ts

// Harness sub-SPEC H6: a function driven through optimizing compiles, speculation exits, a jettison and a
// reoptimization prints counts that the checker finds equal to what the engine printed for its body, each line printed
// where section 10.1 counts. h6Driven adds, so integers train it, doubles and then strings fail its speculations until
// its optimized code is jettisoned and recompiled; noInline keeps every exit and compile of its body under its own name.
// The integrator directory's default turns concurrent JIT off, which fixes every count.
(function main() {
    function h6Driven(a, b) {
        return a + b;
    }
    noInline(h6Driven);
    let sink = 0;
    for (let i = 0; i < 30000; ++i)
        sink += h6Driven(i, 1) & 1;
    for (let i = 0; i < 30000; ++i)
        sink += h6Driven(i + 0.5, 1) & 1;
    for (let i = 0; i < 30000; ++i)
        sink += h6Driven("s", i).length & 1;
    print(JSON.stringify({ name: "h6Driven", counts: jitcacheBodyEvents(h6Driven, "call"), sink }));
})();
