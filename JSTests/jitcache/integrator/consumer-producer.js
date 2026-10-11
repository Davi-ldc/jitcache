// jitcache-runs: Producer; ConsumerProducer; Consumer
// jitcache-runs: Producer; ConsumerProducer --jitcache-max-memory=4096
// jitcache-expect-fault: 1:1 budget.limit

// SPEC-integrator.md section 15.2, consumer-producer.js. Every run of both sequences drives cpBody 40 times with integer
// fields, and the Producer commits it at its compilation, the 34th call.
// - Sequence 0: the ConsumerProducer imports cpBody and installs it at its first call. It then learns what the Producer
//   never saw: doubles, which give cpBody's value profiles new categories once a full collection folds them in, so its
//   jitcacheDelta() commits a richer body for cpBody's key, whose cb.summary changes. The Consumer installs that body,
//   the one whose cb.summary the ConsumerProducer saved in the scratch directory.
// - Sequence 1: the ConsumerProducer's limit of one page ends its production at budget.limit, which run 1 declares:
//   cpFresh, a body only it runs, compiles, and either recording's charge or the capture's is refused (section 4.5).
//   Production ends and activity stays on, so cpBody, which it calls only afterwards, still imports and installs.
// The runner compares every output with the JITCache-off run's, so the work only one role does prints nothing.
load("./resources/integrator.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    function cpBody(point, scale) {
        return point.x * scale + point.y;
    }

    function cpFresh(value) {
        return (value * 7 + 5) | 0;
    }

    function driveBody() {
        let total = 0;
        for (let i = 0; i < 40; ++i)
            total += cpBody({ x: i, y: 1 }, 2);
        return total;
    }

    if (sequence === "1" && role === "ConsumerProducer") {
        let fresh = 0;
        for (let i = 0; i < 40; ++i)
            fresh = (fresh + cpFresh(i)) | 0;
        checkSame(fresh, 5660, "cpFresh's total");
        checkSame(jitcacheStatus(), "budget.limit", "the fault of a ConsumerProducer whose limit is one page, once cpFresh compiled");
        const before = jitcacheProgress();
        const total = driveBody();
        checkSame(grew(before, jitcacheProgress(), "installs"), 1, "the bodies the ConsumerProducer installed after production ended");
        expectInstalled(cpBody);
        print(`cpBody added up to ${total}`);
        return;
    }

    const total = driveBody();
    print(`cpBody added up to ${total}`);

    if (sequence === "0" && role === "ConsumerProducer") {
        expectInstalled(cpBody);
        const imported = hex(sectionOf(cpBody, "call", "cb.summary"));
        check(imported !== null, "cpBody's key holds no body the Producer committed");
        // Ten more calls keep cpBody's baseline counter, which the import carried at no progress, below the DFG's
        // threshold, so cpBody is still a candidate at the delta.
        for (let i = 0; i < 10; ++i)
            cpBody({ x: i + 0.25, y: 1 }, 4);
        fullGC();
        const result = jitcacheDelta();
        check(result.committedBodies >= 1, `the ConsumerProducer's delta committed ${result.committedBodies} bodies after learning doubles`);
        const recaptured = hex(sectionOf(cpBody, "call", "cb.summary"));
        check(recaptured !== imported, "the ConsumerProducer's delta left cpBody's body as the Producer committed it");
        saveText(scratch, "consumer-producer.summary", recaptured);
        return;
    }

    if (sequence === "0" && role === "Consumer") {
        expectInstalled(cpBody);
        checkSame(hex(sectionOf(cpBody, "call", "cb.summary")), loadText(scratch, "consumer-producer.summary"), "the cb.summary of the body the Consumer found");
    }
})(...arguments);
