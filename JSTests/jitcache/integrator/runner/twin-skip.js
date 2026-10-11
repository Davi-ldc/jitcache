// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer --jitcache-test-twin-entry=skip

// Harness sub-SPEC H1: a skip fails the run when concurrent JIT is off in it and in every earlier run that produced, as
// the integrator directory's default leaves it in both runs here (section 7.4).
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
