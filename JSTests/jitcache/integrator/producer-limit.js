// jitcache-runs: Producer --jitcache-max-memory=4096
// jitcache-expect-fault: 0 budget.limit

// SPEC-integrator.md section 15.2, producer-limit.js. A Producer whose limit is one page cannot hold what production
// charges: plBody's compilation records its image, and its capture builds the lanes' sections and needs the writer's
// staging buffer, so whichever of those charges comes first is refused, and every refusal of a production charge is
// raised as budget.limit (section 4.5). Production ends with nothing committed, jitcacheDelta() throws naming the fault,
// and the runner compares the output with the JITCache-off run's.
load("./resources/integrator.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    function plBody(point, scale) {
        return point.x * scale - point.y;
    }

    let total = 0;
    for (let i = 0; i < 40; ++i)
        total += plBody({ x: i, y: 1 }, 3);
    print(`plBody added up to ${total}`);

    if (role !== "Producer")
        return;
    checkSame(jitcacheStatus(), "budget.limit", "the fault of a Producer whose limit is one page");
    checkSame(thrownMessage("jitcacheDelta()", () => jitcacheDelta()), "budget.limit", "the message jitcacheDelta() throws");
    checkSame(jitcacheProgress().capturesCommitted, 0, "the captures a Producer whose limit is one page committed");
    check(!hasBody(plBody), "plBody's key holds a body the limit should have kept out");
})(...arguments);
