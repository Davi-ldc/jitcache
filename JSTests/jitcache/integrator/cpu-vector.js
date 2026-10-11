// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-qemu-cpu: 0 max
// jitcache-qemu-cpu: 1 neoverse-n1
// jitcache-expect-no-install: 1
// jitcache-check: 1 resources/rejected-cpu-vector.ts

// SPEC-integrator.md section 15.2, cpu-vector.js, under QEMU only (harness sub-SPEC section 7.9). The Producer runs on
// QEMU's max model and the Consumer on neoverse-n1, whose CPU feature vectors differ in JSCVT, SHA3 and FRINTTS (harness
// sub-SPEC N27). The header records the Producer's vector (container sub-SPEC section 3.1), so the Consumer's start is
// Rejected at start.incompatible, with a detail naming the lowest differing predicate, which the checker reads in the
// start line. The Consumer's VM stays unconfigured and imports nothing, and the runner compares both outputs with the
// JITCache-off run's.
load("./resources/integrator.js", "caller relative");

(function main(role, scratch, artifact, sequence) {
    function cvBody(value) {
        return (value * 8 - 3) | 0;
    }

    let total = 0;
    for (let i = 0; i < 40; ++i)
        total = (total + cvBody(i)) | 0;
    print(`cvBody added up to ${total}`);

    if (role === "Producer") {
        check(hasBody(cvBody), "cvBody compiled, and its key holds no committed body");
        return;
    }
    if (role === "Consumer") {
        checkSame(jitcacheStartOutcome(), "rejected", "the start of a Consumer on another CPU model");
        checkSame(jitcacheProgress().installs, 0, "the installs of a Consumer whose start was rejected");
    }
})(...arguments);
