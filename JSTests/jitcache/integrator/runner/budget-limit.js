// jitcache-runs: Producer --jitcache-delta-at-exit --jitcache-max-memory=4096
// jitcache-fixture: fails 0:0 fault

// A limit of one page ends the Producer's production at budget.limit, a fault no directive
// declares, which fails run 0.
load("./resources/warm-up.js", "caller relative");
(function main() {
    print(warmUp());
})();
