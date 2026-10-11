// jitcache-runs: Producer --evalMode=true; Consumer
// jitcache-expect-no-install: 1
// jitcache-check: 1 resources/rejected-must-match.ts

// SPEC-integrator.md section 15.2, must-match.js. The Producer runs with evalMode, a must-match option (options.md), on,
// so the header it writes records it (container sub-SPEC section 3.1), and commits mmBody. The Consumer runs with the
// option at its default, off, so its start finds the header incompatible: Rejected at start.incompatible, with a detail
// naming evalMode, which the checker reads in the start line. The Consumer's VM stays unconfigured and imports nothing,
// and the runner compares both outputs with the JITCache-off run's.
load("./resources/integrator.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    function mmBody(value) {
        return (value * 6 + 1) | 0;
    }

    let total = 0;
    for (let i = 0; i < 40; ++i)
        total = (total + mmBody(i)) | 0;
    print(`mmBody added up to ${total}`);

    if (role === "Producer") {
        checkSame(jitcacheStartOutcome(), "started", "the Producer's start");
        check(hasBody(mmBody), "mmBody compiled, and its key holds no committed body");
        return;
    }
    if (role === "Consumer") {
        checkSame(jitcacheStartOutcome(), "rejected", "the start of a Consumer facing a header whose evalMode differs");
        checkSame(jitcacheStatus(), null, "the fault of a Consumer whose start was rejected");
        checkSame(jitcacheProgress().installs, 0, "the installs of a Consumer whose start was rejected");
    }
})(...arguments);
