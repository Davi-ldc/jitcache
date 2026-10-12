// jitcache-runs: Producer --diskCachePath=. --jitcache-delta-at-exit; Consumer --diskCachePath=.
// jitcache-runs: Off --diskCachePath=.; Producer --diskCachePath=. --jitcache-delta-at-exit; Consumer
// jitcache-runs: Producer --diskCachePath=. --jitcache-delta-at-exit; ConsumerProducer --diskCachePath=. --jitcache-delta-at-exit; Consumer
// jitcache-expect-no-install: 1:2
// jitcache-pin: off its runs decode the jsc shell's bytecode cache, whose payload computeJSCBytecodeCacheVersion ties to this build, so the pin's build rejects it and generates where this build decodes

// SPEC-ucb.md section 13.3, context-miss.js: what matches a UCB the native path decoded from its provider's bytecode
// cache (sections 7.3.2 and 7.3.3). The jsc shell decodes a program from the cache --diskCachePath names, which every run
// of a sequence shares (the runs' working directory is the sequence's scratch directory), and writes it when the program's
// source provider dies. Two programs run in realms of their own, so their global vars stay out of the heap the oracle
// compares: one with four declarations, whose declaration maps stay inline, and one with sixty, whose hashed var map a
// decode rebuilds in another layout wherever re-adding its entries in bucket order places them elsewhere (F10).
// - Sequence 0: generated in the Producer and decoded in the Consumer. The decoded functions are seeded; the small
//   program is seeded, and the large one misses CoreDigest where its decoded declaration layout differs, which the
//   Consumer tells by comparing its global var order with the one the Producer saw.
// - Sequence 1: decoded in the Producer, from the cache the Off run wrote, and generated in the Consumer. Every import
//   misses Provenance and nothing attaches, so that Consumer installs nothing.
// - Sequence 2: generated in the Producer, decoded and seeded in a ConsumerProducer, which runs the functions further
//   and recaptures them with jitcacheDelta, then generated in a Consumer, where each recaptured function imports. In
//   twins runs each recaptured function's ucb.identity holds provenance Generated (section 8.2).
load("./resources/ucb.js", "caller relative");
load("./resources/sections.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    const t = ucbTest(role, scratch, artifact, sequence);
    // Which runs decode the programs and which generate them.
    const decodes = t.sequence === 0 ? role === "Consumer" : t.sequence === 1 ? role === "Producer" : role === "ConsumerProducer";

    const declarations = (prefix, count) => Array.from({ length: count }, (_, i) => `${prefix}${i}`);
    const program = (prefix, vars) => [
        `var ${vars.join(", ")};`,
        `function ${prefix}Scale(x) { return (x * 3 + ${vars.length}) | 0; }`,
        `function ${prefix}Label(x) { return "${prefix}:" + x; }`,
        `for (${vars[0]} = 0; ${vars[0]} < 3000; ++${vars[0]}) ${vars[1]} = ((${vars[1]} | 0) + ${vars[0]}) | 0;`,
        `${vars[1]};`,
    ].join("\n");
    const programs = [
        { label: "the program with four declarations", prefix: "cmFew", vars: declarations("cmFewVar", 2) },
        { label: "the program with sixty declarations", prefix: "cmMany", vars: declarations("cmManyVar", 58) },
    ];

    function measured(label, action, expectations)
    {
        const before = t.statistics();
        const value = action();
        const after = t.statistics();
        if (expectations)
            t.expect(label, t.delta(before, after), expectations);
        return value;
    }

    const functions = [];
    for (const entry of programs) {
        const text = program(entry.prefix, entry.vars);
        const before = t.statistics();
        const realm = runString(text);
        const after = t.statistics();
        const order = Object.keys(realm).filter(name => name.startsWith(`${entry.prefix}Var`)).join(" ");
        const orderFile = `context-miss.${entry.prefix}.order`;
        if (!decodes && !t.off)
            t.writeText(orderFile, order);
        if (t.imports) {
            const delta = t.delta(before, after);
            if (!decodes) {
                // Sequence 1 imports bodies the Producer captured from decoded UCBs; sequence 2 may import either program.
                if (t.sequence === 1)
                    t.expect(entry.label, delta, { imports: 0, attaches: 0, "misses.Provenance": 1 });
            } else if (order !== t.readText(orderFile))
                t.expect(`${entry.label}, whose decoded layout differs`, delta, { seededDecodes: 0, "misses.CoreDigest": 1 });
            else
                t.expect(entry.label, delta, { seededDecodes: 1 });
        }

        const functionExpectations = !t.imports ? null
            : decodes ? { seededDecodes: 1 }
            : t.sequence === 1 ? { imports: 0, attaches: 0, "misses.Provenance": 1 }
            : { imports: 1 };
        const scale = realm[`${entry.prefix}Scale`];
        const label = realm[`${entry.prefix}Label`];
        let total = measured(`${entry.prefix}Scale's first call`, () => scale(1), functionExpectations);
        const labelled = measured(`${entry.prefix}Label's first call`, () => label(1), functionExpectations);
        for (let i = 0; i < 400; ++i)
            total = (total + scale(i) + label(i).length) | 0;
        print(`${entry.label}: ${realm[entry.vars[1]]}, ${total}, ${labelled}`);
        functions.push(scale, label);
    }

    // Sequence 1: a second evaluation's CodeCache hit meets the generated program, whose attach never reaches the
    // provenance comparison: the program reached the baseline, so its parked code stops the attach first (section 7.3.4
    // step 2), and once a collection has released that code, the stamp its import's Provenance miss left at the current
    // index token does (step 3). Either way nothing attaches and nothing is digested
    // (SPEC-ucb.md section 13.3, this script's row).
    if (t.sequence === 1)
        measured("evaluating the small program again", () => runString(program(programs[0].prefix, programs[0].vars)), t.imports ? {
            attaches: 0, imports: 0, "misses.Provenance": 0, contextDigests: 0, sourceDigests: 0,
        } : null);

    if (t.sequence === 2 && role === "ConsumerProducer") {
        // Run the seeded functions further, with argument types the Producer never passed, so that the recapture is
        // richer than the saved body and replaces it.
        let further = 0;
        for (let i = 0; i < 400; ++i) {
            further += String(functions[0](i + 0.5)).length;
            further += functions[1]({ toString: () => "object" }).length;
            further += functions[2](i + 0.25);
            further += functions[3](i % 2 ? "string" : null).length;
        }
        t.check(Number.isFinite(further), "the further runs gave no number");
        if (t.twins) {
            const saved = functions.map(fn => t.readSection(t.bodyKey(fn), "ucb.feedback"));
            const result = t.captureDelta();
            t.check(result.committedBodies >= functions.length, () => `jitcacheDelta committed ${t.describe(result)}`);
            functions.forEach((fn, i) => {
                const key = t.bodyKey(fn);
                const feedback = t.readSection(key, "ucb.feedback");
                t.check(saved[i] && feedback && ucbSections.hex(saved[i]) !== ucbSections.hex(feedback), `${fn.name}'s body was not recaptured`);
                const identity = ucbSections.identity(t.readSection(key, "ucb.identity"));
                t.check(identity.provenance === 0, `${fn.name}'s recaptured ucb.identity holds provenance ${identity.provenance}`);
            });
        }
    }
    t.finish();
})(...arguments);
