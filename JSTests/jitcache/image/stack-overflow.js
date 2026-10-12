// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// SPEC-image.md T1 and T2: stack overflow. Unbounded recursion reaches the soft stack limit, which every image's
// prologue compares against (census B1), and jumps to the ThrowStackOverflowAtPrologue thunk (B3); the second function
// declares more parameters than its recursive call passes, so the overflow is found in its arity check, which runs a
// second prologue first. The depth reached depends on how close each run's frames are to the limit, which a skipped
// parse or an earlier tier-up changes, so the script prints only that the stack-overflow RangeError happened (THREAD
// Verification). The consumer checks that its images carry (T1): the soft stack limit (B1); the jump to
// ThrowStackOverflowAtPrologue (B3); the arity fixup's Call (B2); and, in debug builds, which every twins build is, the
// consistency-check routine's tail call to its thunk (B5).
(function main(role, scratch, artifact, sequence) {
    function recurse(depth) {
        return recurse(depth + 1) + 1;
    }
    function recurseWithArity(a, b, c) {
        return recurseWithArity(a) + 1;
    }
    for (const [name, fn] of [["recurse", recurse], ["recurseWithArity", recurseWithArity]]) {
        for (let attempt = 0; attempt < 3; ++attempt) {
            try {
                fn(0);
                print(`${name}: returned`);
            } catch (error) {
                print(`${name}: ${error instanceof RangeError ? "the stack-overflow RangeError" : "an unexpected " + error.name}`);
            }
        }
    }

    // The case happened (HARNESS.md, Tests): each prologue adds to the LLInt's counter, so the producer compiled both
    // functions about a hundred frames deep, the consumer ran every frame in the imported code, and the images carry the
    // targets the header claims.
    if (role === "Consumer") {
        const sections = load("./resources/image-sections.js", "caller relative");
        const bodies = { recurse, recurseWithArity };
        sections.expectImports(role, bodies);
        sections.expectTargets(role, { call: bodies }, [
            { site: "B1", kind: "VMAddress", a: "SoftStackLimit", form: "Pointer" },
            { site: "B3", kind: "CommonThunk", a: "ThrowStackOverflowAtPrologue", form: "Jump" },
            { site: "B2", kind: "ProcessThunk", a: "ArityFixup", form: "Call" },
            { site: "B5", kind: "BaselineThunk", a: "ConsistencyCheck", form: "Jump" },
        ]);
    }
})(...arguments);
