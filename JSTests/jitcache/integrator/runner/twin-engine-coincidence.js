// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-test-twin-entry=coincidence:engine-image
// jitcache-fixture: fails 0:1 twin-coincidence

// A coincidence in the engine image fails the run at once, with no repetition.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
