// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// SPEC-image.md T16 (sections 3.7 and 10.3, I15). Inline switch_string trees of 1 to 64 keys, each a Function-constructor
// body. The producer parses each body before anything else interns its keys, so their atoms are allocated in the order
// the source names them; every other role first uses each key as a property key, last key first, so the same atoms are
// allocated in the reverse order before the bodies are parsed or imported. The trees keep the producer's shape, and the
// consumer writes its own atom at each rank, ordered by its own addresses, and points each rank's case jump at that key's
// case, which T2's twin check compares and the output proves. The switches run with atoms, which take the inline tree,
// with equal strings built at run time, which take operationSwitchStringWithUnknownKeyType, and with strings no case
// names. The consumer checks that the images carry the ranked atoms and case jumps (A16, C13; T1).
(function main(role, scratch, artifact, sequence) {
    const sizes = [1, 2, 3, 4, 7, 8, 9, 16, 31, 32, 33, 48, 63, 64];
    const keyLists = sizes.map(size => {
        const keys = [];
        for (let i = 0; i < size; ++i)
            keys.push("jitcache-rank-" + size + "-" + i);
        return keys;
    });
    const interned = { };
    const intern = keys => {
        for (let i = keys.length; i--;)
            interned[keys[i]] = i;
    };
    if (role !== "Producer")
        keyLists.forEach(intern);

    // The generator emits switch_string only for three case literals or more (CaseBlockNode::tryTableSwitch), and keeps
    // the first of equal keys (BytecodeGenerator::endSwitch), so a table of one or two keys repeats its first key in
    // cases that never run.
    const switches = keyLists.map(keys => {
        const cases = keys.map((key, i) => `case "${key}": return ${i + 1};`);
        while (cases.length < 3)
            cases.push(`case "${keys[0]}": return -1;`);
        return new Function("s", `switch (s) { ${cases.join(" ")} } return 0;`);
    });
    keyLists.forEach(intern);

    const totals = [];
    for (let index = 0; index < sizes.length; ++index) {
        const keys = keyLists[index];
        const fn = switches[index];
        let total = 0;
        for (let call = 0; call < 48; ++call)
            total += fn(keys[(call * 7) % keys.length]) * (call + 1);
        for (let call = 0; call < 4; ++call)
            total += fn([...keys[call % keys.length]].join("")) * 1000;
        total += fn("jitcache-rank-none") + fn(keys.length);
        totals.push(total);
    }
    print(totals.join(" "));

    // The case happened (HARNESS.md, Tests): the producer compiled every switch, each called 54 times, so each ran its
    // import here and met the twin check.
    if (role === "Consumer") {
        const sections = load("./resources/image-sections.js", "caller relative");
        const bodies = { };
        sizes.forEach((size, index) => bodies[`the switch of ${size} keys`] = switches[index]);
        sections.expectImports(role, bodies);
        sections.expectTargets(role, { call: bodies }, [
            { site: "A16", kind: "SwitchStringRankAtom", form: "Pointer" },
            { site: "C13", kind: "SwitchStringRankCase", form: "Jump" },
        ]);
    }
})(...arguments);
