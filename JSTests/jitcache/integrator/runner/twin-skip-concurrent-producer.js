// jitcache-runs: Producer --jitcache-delta-at-exit --useConcurrentJIT=true; Consumer --jitcache-test-twin-entry=skip
// jitcache-fixture: passes
// jitcache-fixture: lists skipped integrator test-entry

// The Producer whose captures the Consumer imports had concurrent JIT on, so a skip in the Consumer is listed and
// passes: a skip fails a run only when concurrent JIT is off in it and in every earlier run that produced.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
