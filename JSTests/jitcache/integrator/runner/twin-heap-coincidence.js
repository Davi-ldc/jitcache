// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-test-twin-entry=coincidence:heap

// Harness sub-SPEC H1: a heap coincidence marks the sequence, whose only failure it is, so the runner repeats the
// sequence once in fresh processes; the test entry recurs there, and the repetition fails.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
