// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-test-twin-entry=difference
// jitcache-fixture: fails 0:1 twin-difference

// The shell writes a difference to the Consumer's twin report right after start, which fails run 1.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
