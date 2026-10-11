// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-test-twin-entry=difference

// Harness sub-SPEC H1: the shell writes a difference to the Consumer's twin report right after start, which fails run 1.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
