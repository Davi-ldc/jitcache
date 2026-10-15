// jitcache-runs: Producer --useConcurrentJIT=false; ConsumerProducer --jitcache-delta-at-exit --useConcurrentJIT=false; Consumer --useConcurrentJIT=false

// A ConsumerProducer's recapture of an image whose MathICs regenerate in two processes. pair's add and mul compile from
// int32 profiles, and every role then gives the add three doubles and the mul three doubles, in that order, and checks
// what pair returns. The producer's add regenerates at its first double, and the producer commits pair with a delta
// between the two runs of doubles, so the body it leaves holds that one regeneration; it takes no delta at exit, and its
// mul's regeneration stays in its process. The ConsumerProducer imports that body, whose record it rebuilds at install
// with the twin data and regeneration log of the producer's: its add already has its full snippet and regenerates no
// more, its mul regenerates, which its record logs too, and it commits the richer body at its delta at exit. The fresh
// consumer imports that body, and its twin check replays both processes' regenerations on a twin compiled from the
// original compilation's inputs, so the restored image, both snippets included, must equal its twin, and the output the
// JITCache-off run's. The producer compiles pair
// at its 34th call, so its doubles reach baseline code, and the two capturing roles keep pair below its tier-up to the
// DFG, so each capture finds it in baseline code.
// In twins builds the ConsumerProducer and the consumer then check that this case happened: pair ran an import in each,
// with no LLInt instruction and no baseline compile, and the body the consumer imported logs two regenerations, the
// producer's of the add and then the ConsumerProducer's of the mul, each MathIC with its snippet. A plain build has no
// delta for a script to call, so there the producer commits pair at its finalize capture alone and the ConsumerProducer
// regenerates both.
(function main(role, scratch, artifact, sequence) {
    function pair(a, b, c, d) {
        return [a + b, c * d];
    }
    noInline(pair);
    const check = (condition, message) => {
        if (!condition)
            throw new Error(message);
    };
    const checkPair = (result, sum, product) => check(result[0] === sum && result[1] === product, `pair gave ${result}, not ${sum},${product}`);

    let total = 0;
    for (let i = 0; i < 40; ++i) {
        const result = pair(i, 1, i, 2);
        total += result[0] + result[1];
    }
    for (let i = 0; i < 3; ++i)
        checkPair(pair(i + 0.5, 1, i, 2), i + 1.5, 2 * i);
    if (role === "Producer" && typeof jitcacheDelta === "function")
        check(jitcacheDelta().committedBodies > 0, "the producer's delta committed no body for pair");
    for (let i = 0; i < 3; ++i)
        checkPair(pair(i, 1, i + 0.5, 2), i + 1, 2 * i + 1);
    print(total, pair(2, 3, 4, 5).join(","));
    const sections = role === "Consumer" || role === "ConsumerProducer" ? load("./resources/image-sections.js", "caller relative") : null;
    if (!sections || !sections.checksImports(role))
        return;

    sections.expectImported(pair);
    if (role !== "Consumer")
        return;
    const key = jitcacheBodyKey(pair, "call");
    const body = key ? sections.readBodySections(key) : null;
    check(body && body.image && body.twins, `pair's key ${key} holds no body with an image and twin data`);
    const mathICs = sections.parseImageSection(body.image).mathICs;
    const regenerated = sections.parseTwinsSection(body.twins).regenerations.map(regeneration => mathICs[regeneration.mathICIndex]);
    const kinds = regenerated.map(mathIC => mathIC ? mathIC.kind : "none");
    check(kinds.length === 2 && kinds[0] === sections.imageMathICKind.Add && kinds[1] === sections.imageMathICKind.Mul,
        `the body the consumer imported logs regenerations of MathIC kinds [${kinds}], not pair's add and then its mul`);
    check(regenerated.every(mathIC => mathIC.snippet), "a MathIC the imported body logs as regenerated has no snippet");
})(...arguments);
