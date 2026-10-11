// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-test-twin-entry=coincidence:engine-image

// Harness sub-SPEC H1: a coincidence in the engine image fails the run at once, with no repetition.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
