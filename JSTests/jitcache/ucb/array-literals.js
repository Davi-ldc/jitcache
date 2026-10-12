// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-runs: Off --diskCachePath=.; Producer --jitcache-delta-at-exit; Consumer --diskCachePath=.
// jitcache-pin: off the second sequence's runs decode the jsc shell's bytecode cache, whose payload computeJSCBytecodeCacheVersion ties to this build, so the pin's build rejects it and generates where this build decodes

// SPEC-ucb.md section 13.3, array-literals.js, F23 and section 7.3.1 step 8: the constant butterflies an array literal's
// generation builds. Each function below holds one literal and searches it with Array.prototype.includes and indexOf:
// - an all-string literal, which generation builds in the atom form (cellButterflyOnlyAtomStringsStructure, each element
//   the VM's canonical JSString for its atom), which the identity section's butterfly map marks and an import rebuilds;
// - strings before a spread with no elision, which builds no butterfly;
// - strings before a spread with a later elision, whose strings form a plain butterfly;
// - a literal with an elision, whose elements before it form a plain butterfly;
// - strings mixed with numbers, a plain butterfly.
// Each is searched for its own strings, for a string no literal holds and for strings built at run time.
// - Sequence 0: generated in the Producer and imported in the Consumer (statistics, imports), where T6 checks every
//   constant's form against the twin's (section 13.2).
// - Sequence 1: the first run, with JITCache off, writes the jsc shell's bytecode cache (--diskCachePath; every run works
//   in its sequence's scratch directory), the Producer generates and captures, and the Consumer decodes the functions
//   from that cache and seeds them (statistics, seededDecodes). Seeding never rebuilds a butterfly, so the decoded ones
//   stay plain, as in the JITCache-off run that decodes them too.
// Every run prints the same results, which the oracle compares with the JITCache-off run.
load("./resources/ucb.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    const decodes = t.sequence === 1 && t.role === "Consumer";
    const rounds = 400;

    function allStrings(s)
    {
        const list = ["alpha", "beta", "gamma", "delta", "epsilon-with-a-longer-name"];
        return `${list.includes(s)}:${list.indexOf(s)}`;
    }
    function stringsBeforeSpread(s, rest)
    {
        const list = ["alpha", "beta", ...rest];
        return `${list.includes(s)}:${list.indexOf(s)}`;
    }
    function stringsBeforeSpreadAndElision(s, rest)
    {
        const list = ["alpha", "beta", ...rest, , "zeta"];
        return `${list.includes(s)}:${list.indexOf(s)}:${list.length}`;
    }
    function withElision(s)
    {
        const list = ["alpha", , "gamma"];
        return `${list.includes(s)}:${list.indexOf(s)}:${list.includes(undefined)}`;
    }
    function mixed(s)
    {
        const list = ["alpha", 1, "beta", 2.5, "gamma"];
        return `${list.includes(s)}:${list.indexOf(s)}:${list.indexOf(2.5)}`;
    }

    // "beta" and "epsilon-with-a-longer-name" built at run time are equal strings that are not the literal's cells; "omega"
    // is in no literal, and neither is the one built from "om" and "ega".
    const built = [["be", "ta"].join(""), ["epsilon", "with", "a", "longer", "name"].join("-"), ["om", "ega"].join("")];
    const searches = ["alpha", "beta", "gamma", "zeta", "omega", ...built];
    const rest = ["gamma", "delta"];
    const calls = [
        ["allStrings", s => allStrings(s)],
        ["stringsBeforeSpread", s => stringsBeforeSpread(s, rest)],
        ["stringsBeforeSpreadAndElision", s => stringsBeforeSpreadAndElision(s, rest)],
        ["withElision", s => withElision(s)],
        ["mixed", s => mixed(s)],
    ];

    for (const [name, call] of calls) {
        const before = t.statistics();
        const first = call(searches[0]);
        const after = t.statistics();
        if (t.imports)
            t.expect(`${name}'s first call`, t.delta(before, after), decodes ? { seededDecodes: [">=", 1] } : { imports: [">=", 1] });

        const results = [first];
        for (let round = 0; round < rounds; ++round) {
            const s = searches[round % searches.length];
            const result = call(s);
            if (round < searches.length)
                results.push(`${s}=${result}`);
        }
        print(`${name}: ${results.join(" ")}`);
    }
    t.finish();
})(...arguments);
