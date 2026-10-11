// jitcache-runs: Producer; Consumer
// jitcache-runs: Producer; Consumer --jitAllowlist=ioAllowed

// SPEC-integrator.md section 15.2, install-outcomes.js: two outcomes of the install function (section 7.3).
// - Sequence 0, a baked-fact mismatch. Every run evaluates one program text twice with loadString. The second evaluation
//   takes the program's UCB from the CodeCache, so the function each evaluation returns, first and second, has an
//   executable of its own over one UCB of ioBody. noDFG(first), before first's first call, makes the CodeBlock first
//   gets CannotCompile, while second's, which compiles in the Producer and is committed then, could compile in the DFG:
//   the capability class is a baked fact (SPEC-image.md section 7). In the Consumer, first's first call imports ioBody,
//   and its newborn CodeBlock meets the mismatch, which counts and keeps the import (step 5); first runs in the LLInt.
//   second's newborn CodeBlock then finds the import kept and installs it, so second's calls add no LLInt instruction to
//   the UCB both share, and nothing compiles ioBody. first's two calls leave the UCB's LLInt counter, which jitSoon
//   armed at the Producer's compilation, short of its threshold of at least 100 points.
// - Sequence 1, the gate. The Consumer's --jitAllowlist names no file, so it is the list of the functions the JIT may
//   compile (FunctionAllowlist): ioAllowed, and not ioGated. ioAllowed installs; ioGated's import fails shouldJIT, the
//   gate the LLInt's tier-up applies, which drops the import (step 6), and ioGated runs in the LLInt, as natively.
// The runner compares every output with the JITCache-off run's.
load("./resources/integrator.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    function bakedFactMismatch() {
        const text = "(function ioBody(value) { return (value * 3 + 1) | 0; })";
        const first = loadString(text);
        const second = loadString(text);
        check(first !== second, "the two evaluations returned one function");
        noDFG(first);

        const before = role === "Consumer" ? jitcacheProgress() : null;
        let total = first(1);
        const afterFirst = role === "Consumer" ? jitcacheProgress() : null;
        total += first(2);
        for (let i = 0; i < 40; ++i)
            total = (total + second(i)) | 0;
        print(`ioBody added up to ${total}`);

        if (role === "Producer")
            check(hasBody(second), "ioBody compiled, and its key holds no committed body");
        if (role !== "Consumer")
            return;
        checkSame(grew(before, afterFirst, "bakedFactMismatches"), 1, "the baked-fact mismatches of first's newborn CodeBlock");
        checkSame(grew(before, afterFirst, "installs"), 0, "the installs of first's newborn CodeBlock");
        const afterSecond = jitcacheProgress();
        checkSame(grew(afterFirst, afterSecond, "installs"), 1, "the installs of second's newborn CodeBlock");
        checkSame(grew(afterFirst, afterSecond, "bakedFactMismatches"), 0, "the baked-fact mismatches of second's newborn CodeBlock");
        // first and second share ioBody's UCB, which counts the events of both.
        const counts = events(second);
        check(counts.llintInstructions > 0, "first's calls began no LLInt instruction of ioBody");
        checkSame(counts.llintInstructions, events(first).llintInstructions, "the LLInt instructions of the UCB first and second share");
        checkSame(counts.baselineCompiles, 0, "ioBody's baseline compiles once second installed the kept import");
    }

    function gate() {
        function ioAllowed(value) {
            return (value * 5 + 2) | 0;
        }
        function ioGated(value) {
            return (value * 11 + 3) | 0;
        }

        const before = role === "Consumer" ? jitcacheProgress() : null;
        let total = 0;
        for (let i = 0; i < 40; ++i)
            total = (total + ioAllowed(i)) | 0;
        const afterAllowed = role === "Consumer" ? jitcacheProgress() : null;
        for (let i = 0; i < 40; ++i)
            total = (total + ioGated(i)) | 0;
        print(`ioAllowed and ioGated added up to ${total}`);

        if (role === "Producer") {
            check(hasBody(ioAllowed), "ioAllowed compiled, and its key holds no committed body");
            check(hasBody(ioGated), "ioGated compiled, and its key holds no committed body");
        }
        if (role !== "Consumer")
            return;
        checkSame(grew(before, afterAllowed, "installs"), 1, "the installs of ioAllowed, which the allowlist names");
        const afterGated = jitcacheProgress();
        checkSame(grew(afterAllowed, afterGated, "gateDrops"), 1, "the imports the gate dropped at ioGated");
        checkSame(grew(afterAllowed, afterGated, "installs"), 0, "the installs of ioGated, which the allowlist leaves out");
        expectInstalled(ioAllowed);
        const gated = events(ioGated);
        check(gated.llintInstructions > 0, "ioGated, whose import the gate dropped, began no LLInt instruction");
        checkSame(gated.baselineCompiles, 0, "the baseline compiles of ioGated, which the allowlist leaves out");
    }

    if (sequence === "0")
        bakedFactMismatch();
    else
        gate();
})(...arguments);
