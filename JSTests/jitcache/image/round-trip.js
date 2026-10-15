// jitcache-requires: twins
// jitcache-runs: Producer --useConcurrentJIT=false; ConsumerProducer --useConcurrentJIT=false

// An imported image recaptured before anything changes it comes back byte for byte. The producer commits body at its
// finalize capture, with cold ICs and no counter progress. The ConsumerProducer imports it, rebuilding its image record
// from the sections at install, runs it with the
// same int32s, so no MathIC regenerates and no tier-up check rewrites the coverage rates, and recaptures it with delta,
// which commits because the ICs now hold cases and the counter has progressed. The recapture's image.baseline and
// baked-facts.baseline equal the imported ones byte for byte, and its image-twins.baseline equals the imported one except
// the capture-process token and the producer values, which hold the capturing VM's own resolutions. The ConsumerProducer
// first checks that body ran its import, with no LLInt instruction and no baseline compile, so the recapture is one of
// the imported image. The jitcache functions run only in the ConsumerProducer, so the pin comparison runs the script's
// off path.
(function main(role, scratch, artifact, sequence) {
    function body(object, n) {
        const label = "round-trip";
        if (object.count > n)
            return label.length + object.count * 2 - n;
        return object.items[n % object.items.length] + object.count;
    }

    const object = { count: 7, items: [1, 2, 3] };
    let total = 0;
    for (let i = 0; i < 40; ++i)
        total += body(object, i);
    print(total);
    if (role !== "ConsumerProducer")
        return;

    const sections = load("./resources/image-sections.js", "caller relative");
    sections.expectImported(body);
    const key = jitcacheBodyKey(body, "call");
    if (!key)
        throw new Error("body has no key");
    const imported = sections.readBodySections(key);
    if (!imported.image || !imported.bakedFacts || !imported.twins || !imported.cbState)
        throw new Error("the producer committed no complete body");
    const result = jitcacheDelta();
    const recaptured = sections.readBodySections(key);
    if (!result.committedBodies || sections.sameImageBytes(imported.cbState, recaptured.cbState))
        throw new Error(`delta committed no recapture of body: ${JSON.stringify(result)}`);
    if (!sections.sameImageBytes(imported.image, recaptured.image))
        throw new Error("the recaptured image.baseline differs from the imported one");
    if (!sections.sameImageBytes(imported.bakedFacts, recaptured.bakedFacts))
        throw new Error("the recaptured baked-facts.baseline differs from the imported one");
    if (!sections.sameTwinsExceptCapturingProcess(imported.twins, recaptured.twins))
        throw new Error("the recaptured image-twins.baseline differs from the imported one outside its token and producer values");
})(...arguments);
