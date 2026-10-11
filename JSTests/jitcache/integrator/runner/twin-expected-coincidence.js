// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-test-twin-entry=coincidence:executable-pool
// jitcache-expect-twin: 1 coincidence executable-pool

// Harness sub-SPEC H1: twin-pool-coincidence.js with the coincidence declared, which neither fails nor marks anything.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
