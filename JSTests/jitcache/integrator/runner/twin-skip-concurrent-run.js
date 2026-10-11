// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-test-twin-entry=skip --useConcurrentJIT=true

// Harness sub-SPEC H1: the run that reports the skip has concurrent JIT on, so the runner lists the skip and passes.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
