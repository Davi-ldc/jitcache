// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=true; Consumer --jitcache-test-twin-entry=skip

// Harness sub-SPEC H1: the Producer whose captures the Consumer imports had concurrent JIT on, so a skip in the
// Consumer is listed and passes, as section 7.4's rule gives.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
