// jitcache-runs: Producer --jitcache-delta-at-exit; ConsumerProducer --jitcache-test-store-fault=scoring:EMFILE; Consumer

// SPEC-integrator.md section 15.2, scoring-deferral.js. The Producer commits sdFirst and sdSecond. In the
// ConsumerProducer the store's fault hook fails every scoring read at its openat with EMFILE, an error that says nothing
// about the artifact (section 8.3), so every key it scores is deferred, with no fault and no commit, whether or not the
// key has a body. sdFirst and sdSecond import and install at their first calls. sdFresh, which only the ConsumerProducer
// runs, compiles there and gets no body; its compilation's capture is already deferred. The ConsumerProducer then warms
// the imported bodies further, with doubles, and folds the samples in with a full collection, so their candidates would
// beat the saved bodies, and calls jitcacheDelta(): every eligible key is deferred (the result's deferredKeys, and as
// many more capturesDeferred), nothing is committed, no fault is raised, and the cb.summary bytes of both imported keys
// are what they were before the call. The Consumer imports the Producer's bodies. The runner compares every output with
// the JITCache-off run's.
load("./resources/integrator.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    function sdFirst(point) {
        return point.x * 3 + point.y;
    }

    function sdSecond(a, b) {
        return a * b - 1;
    }

    function sdFresh(value) {
        return (value * 17 + 1) | 0;
    }

    let total = 0;
    for (let i = 0; i < 40; ++i) {
        total += sdFirst({ x: i, y: 2 });
        total += sdSecond(i, 3);
    }
    print(`sdFirst and sdSecond added up to ${total}`);

    if (role === "Consumer") {
        expectInstalled(sdFirst);
        expectInstalled(sdSecond);
        return;
    }
    if (role !== "ConsumerProducer")
        return;

    expectInstalled(sdFirst);
    expectInstalled(sdSecond);
    let fresh = 0;
    for (let i = 0; i < 40; ++i)
        fresh = (fresh + sdFresh(i)) | 0;
    checkSame(fresh, 13300, "sdFresh's total");
    check(!hasBody(sdFresh), "sdFresh, whose capture the failing scoring read deferred, has a body");

    // Ten more calls of each keep both imported counters, which the Producer's delta carried, below the DFG's threshold,
    // so both are still candidates at the delta.
    for (let i = 0; i < 10; ++i) {
        sdFirst({ x: i + 0.5, y: 2 });
        sdSecond(i + 0.5, 3);
    }
    fullGC();

    const summariesBefore = [hex(sectionOf(sdFirst, "call", "cb.summary")), hex(sectionOf(sdSecond, "call", "cb.summary"))];
    check(summariesBefore[0] !== null && summariesBefore[1] !== null, "an imported key holds no body");
    const before = jitcacheProgress();
    check(before.capturesDeferred >= 1, "the capture at sdFresh's compilation was not deferred");
    const result = jitcacheDelta();
    const after = jitcacheProgress();

    checkSame(result.committedBodies, 0, "the bodies a delta whose every scoring read failed committed");
    check(result.eligibleKeys >= 3, `the delta found ${result.eligibleKeys} eligible keys, fewer than sdFirst, sdSecond and sdFresh`);
    checkSame(result.deferredKeys, result.eligibleKeys, "the keys the delta deferred");
    checkSame(grew(before, after, "capturesDeferred"), result.deferredKeys, "the captures the delta deferred");
    checkSame(after.capturesCommitted, 0, "the captures the ConsumerProducer committed");
    checkSame(jitcacheStatus(), null, "the ConsumerProducer's fault");
    checkSame(hex(sectionOf(sdFirst, "call", "cb.summary")), summariesBefore[0], "sdFirst's cb.summary after the delta");
    checkSame(hex(sectionOf(sdSecond, "call", "cb.summary")), summariesBefore[1], "sdSecond's cb.summary after the delta");
})(...arguments);
