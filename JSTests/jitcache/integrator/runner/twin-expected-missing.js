// jitcache-runs: Producer --jitcache-delta-at-exit; Consumer
// jitcache-expect-twin: 1 coincidence executable-pool

// A declared report that never appears fails the run that declares it.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
