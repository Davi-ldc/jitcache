// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// SPEC-cb.md section 11.3, the realm step of section 5.2. A body whose for-of saw only maps and sets in the Producer
// carries FastMap and FastSet at its iterator_open. The Consumer imports it and DFG-compiles it before its realm creates
// any Map or Set, so the DFG's FastMap and FastSet cases find mapProtoEntriesFunction and setProtoValuesFunction only
// because seedLinkedState materialized them (N10); in debug-local the DFG asserts that they exist. One body lives in the
// shell's realm and one in a realm made by createGlobalObject, whose functions are fresh. Only after both compile does
// either realm create a map or a set, which the compiled code then iterates. Concurrent JIT is off in plain mode too, so
// each compilation installs within the call that requests it.
(function main(role) {
    // The other realm's copy, compiled there by its indirect eval; it differs from the shell realm's copy in name, so
    // the two bodies have different keys.
    const thereSource = `(function realmIterateThere(iterable) {
        let total = 0;
        for (const item of iterable)
            total += typeof item === "number" ? item : item[0] + item[1];
        return total;
    })`;
    function realmIterateHere(iterable) {
        let total = 0;
        for (const item of iterable)
            total += typeof item === "number" ? item : item[0] + item[1];
        return total;
    }
    const other = createGlobalObject();
    const realmIterateThere = other.eval(thereSource);
    // Each body compiles on its own, and no caller's compilation inlines it.
    noInline(realmIterateHere);
    noInline(realmIterateThere);

    if (role === "Producer") {
        // Maps and sets of each body's own realm, the only iterables that record FastMap and FastSet there
        // (getIterationMode compares the iterable's iterator function with its realm's). Sixty calls compile each body to
        // baseline and stop short of the DFG, and the baseline compilation's capture already holds the modes.
        const here = [new Map([[1, 2], [3, 4]]), new Set([5, 6])];
        const there = [new other.Map([[1, 2], [3, 4]]), new other.Set([5, 6])];
        for (let i = 0; i < 60; ++i) {
            realmIterateHere(here[i & 1]);
            realmIterateThere(there[i & 1]);
        }
        if (typeof jitcacheDelta === "function") {
            jitcacheDelta();
            const sections = load("./resources/cb-sections.js", "caller relative");
            const fastMapAndSet = sections.IterationMode.FastMap | sections.IterationMode.FastSet;
            for (const [fn, label] of [[realmIterateHere, "realmIterateHere"], [realmIterateThere, "realmIterateThere"]]) {
                const [record] = sections.entries(sections.checkBody(fn, "call", label).state, 20);
                if (!record || (record.modes & fastMapAndSet) !== fastMapAndSet)
                    throw new Error(`${label}: iterator_open records ${sections.describe(record)}, expected FastMap and FastSet`);
            }
        }
    }

    // Arrays only, until both bodies have optimized code. The first call imports each body in the Consumer.
    const numbers = [1, 2, 3];
    const otherNumbers = other.eval("[1, 2, 3]");
    for (let i = 0; i < 100000 && !(numberOfDFGCompiles(realmIterateHere) && numberOfDFGCompiles(realmIterateThere)); ++i) {
        realmIterateHere(numbers);
        realmIterateThere(otherNumbers);
    }
    if (!numberOfDFGCompiles(realmIterateHere) || !numberOfDFGCompiles(realmIterateThere))
        throw new Error("a realm's body never reached the DFG");

    if (role === "Consumer" && typeof jitcacheBodyEvents === "function") {
        const sections = load("./resources/cb-sections.js", "caller relative");
        sections.expectImported(realmIterateHere, "call", "realmIterateHere");
        sections.expectImported(realmIterateThere, "call", "realmIterateThere");
    }

    // Now each realm makes a map and a set, and the optimized code iterates them. Their iterator functions are the ones
    // the realm step materialized, which `new Map` and `new Set` installed on the prototypes.
    const results = [
        realmIterateHere(new Map([[10, 20], [30, 40]])),
        realmIterateHere(new Set([50, 60])),
        realmIterateThere(new other.Map([[10, 20], [30, 40]])),
        realmIterateThere(new other.Set([50, 60])),
        realmIterateHere(numbers),
        realmIterateThere(otherNumbers),
        Map.prototype[Symbol.iterator] === Map.prototype.entries,
        Set.prototype[Symbol.iterator] === Set.prototype.values,
        other.Map.prototype[Symbol.iterator] === other.Map.prototype.entries,
        other.Set.prototype[Symbol.iterator] === other.Set.prototype.values,
    ];
    print(JSON.stringify(results));
})(...arguments);
