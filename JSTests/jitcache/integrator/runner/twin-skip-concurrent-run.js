// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-test-twin-entry=skip --useConcurrentJIT=true
// jitcache-fixture: passes
// jitcache-fixture: lists skipped integrator test-entry

// The run that reports the skip has concurrent JIT on, so the runner lists the skip and passes.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
