// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-test-twin-entry=coincidence:executable-pool
// jitcache-fixture: fails 0:1 twin-coincidence

// Placement moved the executable pool, so a coincidence there fails the run at once, with no
// repetition.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
