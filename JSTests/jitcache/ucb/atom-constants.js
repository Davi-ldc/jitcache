// String constants longer than an inline string, compared with === and !==, whose baseline fast path compares an atom
// constant by pointer. Generation makes every string constant an atom, and an import decodes each as one, so every string
// constant register of an imported UCB is an atom before its first CodeBlock: the twin check compares the imported core
// with its twin's, atoms included, and each install's image with its twin compile. Each function compares its own
// constant as a value or as a branch; the one on the left is a template literal, since generation moves a string literal
// on the left of === to the right, while a template literal stays the instruction's left operand. The alpha constant is
// also a property name, read with get_by_id in acKeyedAlpha and written as a computed key by its caller.
// The Producer runs every function until it compiles, which captures it with its image; the Consumer imports each at its
// first call (statistics) and installs it. The runner turns concurrent JIT off in both runs, so a twin check that skips
// itself fails the run. Every run prints the same results, which the oracle compares with the JITCache-off run.
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);

    function acEqualsAlpha(s)
    {
        return s === "atom-constants-alpha-longer-than-inline";
    }
    function acNotEqualsBeta(s)
    {
        return s !== "atom-constants-beta-longer-than-inline";
    }
    function acBranchGamma(s)
    {
        if ("atom-constants-gamma-longer-than-inline" === s)
            return 1;
        return 0;
    }
    function acLeftDelta(s)
    {
        return `atom-constants-delta-longer-than-inline` === s;
    }
    function acKeyedAlpha(o)
    {
        return o["atom-constants-alpha-longer-than-inline"];
    }

    // Equal strings built at run time, which are not the constants' cells, beside the constants' own characters and a
    // string no function holds.
    const built = name => ["atom-constants", name, "longer-than-inline"].join("-");
    const inputs = [built("alpha"), built("beta"), built("gamma"), built("delta"), "atom-constants-alpha-longer-than-inline", "other"];
    const calls = [
        ["acEqualsAlpha", s => acEqualsAlpha(s)],
        ["acNotEqualsBeta", s => acNotEqualsBeta(s)],
        ["acBranchGamma", s => acBranchGamma(s)],
        ["acLeftDelta", s => acLeftDelta(s)],
        ["acKeyedAlpha", s => acKeyedAlpha({ [s]: s.length })],
    ];

    for (const [name, call] of calls) {
        const before = t.statistics();
        const first = call(inputs[0]);
        const after = t.statistics();
        // The arrow that makes the call ran hot in the Producer as well, so its body may import beside the function's.
        if (t.imports)
            t.expect(`${name}'s first call`, t.delta(before, after), { imports: [">=", 1] });

        const results = [first];
        for (let round = 0; round < 400; ++round) {
            const result = call(inputs[round % inputs.length]);
            if (round < inputs.length)
                results.push(result);
        }
        print(`${name}: ${results.join(" ")}`);
    }
    t.finish();
})(...arguments);
